// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <magenta/device/vfs.h>
#include <unistd.h>

#include <memory>
#include <utility>

#include "application/lib/app/application_context.h"
#include "apps/ledger/services/internal/internal.fidl.h"
#include "apps/ledger/src/app/erase_remote_repository_operation.h"
#include "apps/ledger/src/app/ledger_repository_factory_impl.h"
#include "apps/ledger/src/backoff/exponential_backoff.h"
#include "apps/ledger/src/cobalt/cobalt.h"
#include "apps/ledger/src/environment/environment.h"
#include "apps/ledger/src/network/network_service_impl.h"
#include "apps/ledger/src/network/no_network_service.h"
#include "apps/network/services/network_service.fidl.h"
#include "apps/tracing/lib/trace/provider.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/ftl/command_line.h"
#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/log_settings_command_line.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/time/time_delta.h"
#include "lib/ftl/time/time_point.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {

namespace {

constexpr ftl::StringView kPersistentFileSystem = "/data";
constexpr ftl::StringView kMinFsName = "minfs";
constexpr ftl::TimeDelta kMaxPollingDelay = ftl::TimeDelta::FromSeconds(10);
constexpr ftl::StringView kNoMinFsFlag = "no_minfs_wait";
constexpr ftl::StringView kNoPersistedConfig = "no_persisted_config";
constexpr ftl::StringView kNoNetworkForTesting = "no_network_for_testing";
constexpr ftl::StringView kNoStatisticsReporting =
    "no_statistics_reporting_for_testing";
constexpr ftl::StringView kTriggerCloudErasedForTesting =
    "trigger_cloud_erased_for_testing";

struct AppParams {
  LedgerRepositoryFactoryImpl::ConfigPersistence config_persistence =
      LedgerRepositoryFactoryImpl::ConfigPersistence::PERSIST;
  bool no_network_for_testing = false;
  bool trigger_cloud_erased_for_testing = false;
  bool disable_statistics = false;
};

ftl::AutoCall<ftl::Closure> SetupCobalt(
    bool disable_statistics,
    ftl::RefPtr<ftl::TaskRunner> task_runner,
    app::ApplicationContext* application_context) {
  if (disable_statistics) {
    return ftl::MakeAutoCall<ftl::Closure>([] {});
  }
  return InitializeCobalt(std::move(task_runner), application_context);
};

// App is the main entry point of the Ledger application.
//
// It is responsible for setting up the LedgerRepositoryFactory, which connects
// clients to individual Ledger instances. It should not however hold long-lived
// objects shared between Ledger instances, as we need to be able to put them in
// separate processes when the app becomes multi-instance.
class App : public LedgerController,
            public LedgerRepositoryFactoryImpl::Delegate {
 public:
  explicit App(AppParams app_params)
      : app_params_(std::move(app_params)),
        application_context_(app::ApplicationContext::CreateFromStartupInfo()),
        cobalt_cleaner_(SetupCobalt(app_params_.disable_statistics,
                                    loop_.task_runner(),
                                    application_context_.get())),
        config_persistence_(app_params_.config_persistence) {
    FTL_DCHECK(application_context_);
    tracing::InitializeTracer(application_context_.get(), {"ledger"});

    ReportEvent(CobaltEvent::LEDGER_STARTED);
  }
  ~App() override {}

  bool Start() {
    if (app_params_.no_network_for_testing) {
      network_service_ =
          std::make_unique<ledger::NoNetworkService>(loop_.task_runner());
    } else {
      network_service_ = std::make_unique<ledger::NetworkServiceImpl>(
          loop_.task_runner(), [this] {
            return application_context_
                ->ConnectToEnvironmentService<network::NetworkService>();
          });
    }
    environment_ = std::make_unique<Environment>(loop_.task_runner(),
                                                 network_service_.get());
    if (app_params_.trigger_cloud_erased_for_testing) {
      environment_->SetTriggerCloudErasedForTesting();
    }

    factory_impl_ = std::make_unique<LedgerRepositoryFactoryImpl>(
        this, environment_.get(), config_persistence_);

    application_context_->outgoing_services()
        ->AddService<LedgerRepositoryFactory>(
            [this](fidl::InterfaceRequest<LedgerRepositoryFactory> request) {
              factory_bindings_.AddBinding(factory_impl_.get(),
                                           std::move(request));
            });
    application_context_->outgoing_services()->AddService<LedgerController>(
        [this](fidl::InterfaceRequest<LedgerController> request) {
          controller_bindings_.AddBinding(this, std::move(request));
        });

    loop_.Run();

    return true;
  }

 private:
  // LedgerController implementation.
  void Terminate() override {
    // Wait for pending asynchronous operations on the
    // LedgerRepositoryFactoryImpl, such as erasing a repository, but do not
    // allow new requests to be started in the meantime.
    shutdown_in_progress_ = true;
    factory_bindings_.CloseAllBindings();
    application_context_->outgoing_services()->Close();
    factory_impl_.reset();

    if (pending_operation_manager_.size() == 0u) {
      // If we still have pending operations, we will post the quit task when
      // the last one completes.
      loop_.PostQuitTask();
    }
  }

  // LedgerRepositoryFactoryImpl::Delegate:
  void EraseRepository(
      EraseRemoteRepositoryOperation erase_remote_repository_operation,
      std::function<void(bool)> callback) override {
    auto handler = pending_operation_manager_.Manage(
        std::move(erase_remote_repository_operation));
    handler.first->Start([
      this, cleanup = std::move(handler.second), callback = std::move(callback)
    ](bool succeeded) {
      callback(succeeded);
      // This lambda is deleted in |cleanup()|, don't access captured members
      // afterwards.
      cleanup();
      CheckPendingOperations();
    });
  }

  void CheckPendingOperations() {
    if (shutdown_in_progress_ && pending_operation_manager_.size() == 0u) {
      loop_.PostQuitTask();
    }
  }

  bool shutdown_in_progress_ = false;
  mtl::MessageLoop loop_;
  const AppParams app_params_;
  std::unique_ptr<app::ApplicationContext> application_context_;
  ftl::AutoCall<ftl::Closure> cobalt_cleaner_;
  const LedgerRepositoryFactoryImpl::ConfigPersistence config_persistence_;
  std::unique_ptr<NetworkService> network_service_;
  std::unique_ptr<Environment> environment_;
  std::unique_ptr<LedgerRepositoryFactoryImpl> factory_impl_;
  fidl::BindingSet<LedgerRepositoryFactory> factory_bindings_;
  fidl::BindingSet<LedgerController> controller_bindings_;
  callback::PendingOperationManager pending_operation_manager_;

  FTL_DISALLOW_COPY_AND_ASSIGN(App);
};

void WaitForData() {
  backoff::ExponentialBackoff backoff(ftl::TimeDelta::FromMilliseconds(10), 2,
                                      ftl::TimeDelta::FromSeconds(1));
  ftl::TimePoint now = ftl::TimePoint::Now();
  while (ftl::TimePoint::Now() - now < kMaxPollingDelay) {
    ftl::UniqueFD fd(open(kPersistentFileSystem.data(), O_RDWR));
    FTL_DCHECK(fd.is_valid());
    char buf[sizeof(vfs_query_info_t) + MAX_FS_NAME_LEN + 1];
    vfs_query_info_t* info = reinterpret_cast<vfs_query_info_t*>(buf);
    ssize_t len = ioctl_vfs_query_fs(fd.get(), info, sizeof(buf) - 1);
    FTL_DCHECK(len > static_cast<ssize_t>(sizeof(vfs_query_info_t)));
    ftl::StringView fs_name(info->name, len - sizeof(vfs_query_info_t));

    if (fs_name == kMinFsName) {
      return;
    }

    usleep(backoff.GetNext().ToMicroseconds());
  }

  FTL_LOG(WARNING) << kPersistentFileSystem
                   << " is not persistent. Did you forget to configure it?";
}

}  // namespace
}  // namespace ledger

int main(int argc, const char** argv) {
  const auto command_line = ftl::CommandLineFromArgcArgv(argc, argv);
  ftl::SetLogSettingsFromCommandLine(command_line);

  ledger::AppParams app_params;
  if (command_line.HasOption(ledger::kNoPersistedConfig)) {
    app_params.config_persistence =
        ledger::LedgerRepositoryFactoryImpl::ConfigPersistence::FORGET;
  }
  app_params.no_network_for_testing =
      command_line.HasOption(ledger::kNoNetworkForTesting);
  app_params.trigger_cloud_erased_for_testing =
      command_line.HasOption(ledger::kTriggerCloudErasedForTesting);
  app_params.disable_statistics =
      command_line.HasOption(ledger::kNoStatisticsReporting);

  if (!command_line.HasOption(ledger::kNoMinFsFlag.ToString())) {
    // Poll until /data is persistent. This is need to retrieve the Ledger
    // configuration.
    ledger::WaitForData();
  }

  ledger::App app(std::move(app_params));
  if (!app.Start()) {
    return 1;
  }

  return 0;
}
