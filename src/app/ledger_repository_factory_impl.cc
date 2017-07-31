// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/ledger_repository_factory_impl.h"

#include "apps/ledger/src/app/auth_provider_impl.h"
#include "apps/ledger/src/app/constants.h"
#include "apps/ledger/src/backoff/exponential_backoff.h"
#include "apps/ledger/src/cloud_sync/impl/cloud_device_set_impl.h"
#include "apps/ledger/src/cloud_sync/impl/paths.h"
#include "apps/ledger/src/cloud_sync/impl/user_sync_impl.h"
#include "apps/tracing/lib/trace/event.h"
#include "lib/ftl/files/directory.h"
#include "lib/ftl/files/file.h"
#include "lib/ftl/files/path.h"
#include "lib/ftl/files/scoped_temp_dir.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/strings/concatenate.h"
#include "lib/ftl/strings/string_view.h"

namespace ledger {

constexpr ftl::StringView kContentPath = "/content";
constexpr ftl::StringView kStagingPath = "/staging";

namespace {
cloud_sync::UserConfig GetUserConfig(Environment* environment,
                                     const FirebaseConfigPtr& firebase_config,
                                     ftl::StringView user_id,
                                     ftl::StringView user_directory,
                                     cloud_sync::AuthProvider* auth_provider) {
  FTL_DCHECK(firebase_config);
  cloud_sync::UserConfig user_config;
  user_config.use_sync = true;
  user_config.server_id = firebase_config->server_id.get();
  user_config.user_id = user_id.ToString();
  user_config.user_directory = user_directory.ToString();
  user_config.auth_provider = auth_provider;
  auto user_firebase = std::make_unique<firebase::FirebaseImpl>(
      environment->network_service(), user_config.server_id,
      cloud_sync::GetFirebasePathForUser(user_config.user_id));
  user_config.cloud_device_set =
      std::make_unique<cloud_sync::CloudDeviceSetImpl>(
          std::move(user_firebase));
  return user_config;
}

bool SaveConfigForDebugging(ftl::StringView user_id,
                            ftl::StringView repository_path,
                            const std::string& temp_dir) {
  if (!files::WriteFileInTwoPhases(kLastUserIdPath.ToString(), user_id,
                                   temp_dir)) {
    return false;
  }
  if (!files::WriteFileInTwoPhases(kLastUserRepositoryPath.ToString(),
                                   repository_path, temp_dir)) {
    return false;
  }
  return true;
}

bool GetRepositoryName(const fidl::String& repository_path, std::string* name) {
  std::string name_path = repository_path.get() + "/name";

  if (files::ReadFileToString(name_path, name)) {
    return true;
  }

  if (!files::CreateDirectory(repository_path.get())) {
    return false;
  }

  std::string new_name;
  new_name.resize(16);
  glue::RandBytes(&new_name[0], new_name.size());
  if (!files::WriteFile(name_path, new_name.c_str(), new_name.size())) {
    FTL_LOG(ERROR) << "Unable to write file at: " << name_path;
    return false;
  }

  name->swap(new_name);
  return true;
}

}  // namespace

// Container for a LedgerRepositoryImpl that keeps tracks of the in-flight FIDL
// requests and callbacks and fires them when the repository is available.
// TODO(ppi): LE-224 extract this into a generic class shared with
// LedgerManager.
class LedgerRepositoryFactoryImpl::LedgerRepositoryContainer {
 public:
  explicit LedgerRepositoryContainer(
      std::unique_ptr<cloud_sync::AuthProvider> auth_provider)
      : status_(Status::OK), auth_provider_(std::move(auth_provider)) {}
  ~LedgerRepositoryContainer() {
    for (const auto& request : requests_) {
      request.second(Status::INTERNAL_ERROR);
    }
  }

  void set_on_empty(const ftl::Closure& on_empty_callback) {
    on_empty_callback_ = on_empty_callback;
    if (ledger_repository_) {
      ledger_repository_->set_on_empty(on_empty_callback);
    }
  };

  // Keeps track of |request| and |callback|. Binds |request| and fires
  // |callback| when the repository is available or an error occurs.
  void BindRepository(fidl::InterfaceRequest<LedgerRepository> request,
                      std::function<void(Status)> callback) {
    if (status_ != Status::OK) {
      callback(status_);
      return;
    }
    if (ledger_repository_) {
      ledger_repository_->BindRepository(std::move(request));
      callback(status_);
      return;
    }
    requests_.emplace_back(std::move(request), std::move(callback));
  }

  // Sets the implementation or the error status for the container. This
  // notifies all awaiting callbacks and binds all pages in case of success.
  void SetRepository(Status status,
                     std::unique_ptr<LedgerRepositoryImpl> ledger_repository) {
    FTL_DCHECK(!ledger_repository_);
    FTL_DCHECK(status != Status::OK || ledger_repository);
    status_ = status;
    ledger_repository_ = std::move(ledger_repository);
    for (auto& request : requests_) {
      if (ledger_repository_) {
        ledger_repository_->BindRepository(std::move(request.first));
      }
      request.second(status_);
    }
    requests_.clear();
    if (on_empty_callback_) {
      if (ledger_repository_) {
        ledger_repository_->set_on_empty(on_empty_callback_);
      } else {
        on_empty_callback_();
      }
    }
  }

  // Shuts down the repository impl (if already initialized) and detaches all
  // handles bound to it, moving their owneship to the container.
  void Detach() {
    if (ledger_repository_) {
      detached_handles_ = ledger_repository_->Unbind();
      ledger_repository_.reset();
    }
    for (auto& request : requests_) {
      detached_handles_.push_back(std::move(request.first));
    }

    // TODO(ppi): rather than failing all already pending and future requests,
    // we should stash them and fullfill them once the deletion is finished.
    status_ = Status::INTERNAL_ERROR;
  }

 private:
  std::unique_ptr<LedgerRepositoryImpl> ledger_repository_;
  Status status_;
  std::unique_ptr<cloud_sync::AuthProvider> auth_provider_;
  std::vector<std::pair<fidl::InterfaceRequest<LedgerRepository>,
                        std::function<void(Status)>>>
      requests_;
  ftl::Closure on_empty_callback_;
  std::vector<fidl::InterfaceRequest<LedgerRepository>> detached_handles_;

  FTL_DISALLOW_COPY_AND_ASSIGN(LedgerRepositoryContainer);
};

struct LedgerRepositoryFactoryImpl::RepositoryInformation {
 public:
  explicit RepositoryInformation(const fidl::String& repository_path)
      : base_path(files::SimplifyPath(repository_path.get())),
        content_path(ftl::Concatenate({base_path, kContentPath})),
        staging_path(ftl::Concatenate({base_path, kStagingPath})) {}

  RepositoryInformation(const RepositoryInformation& other) = default;
  RepositoryInformation(RepositoryInformation&& other) = default;
  RepositoryInformation& operator=(const RepositoryInformation& other) =
      default;

  bool Init() { return GetRepositoryName(content_path, &name); }

  const std::string base_path;
  const std::string content_path;
  const std::string staging_path;
  std::string name;
};

LedgerRepositoryFactoryImpl::LedgerRepositoryFactoryImpl(
    Delegate* delegate,
    ledger::Environment* environment,
    ConfigPersistence config_persistence)
    : delegate_(delegate),
      environment_(environment),
      config_persistence_(config_persistence) {}

LedgerRepositoryFactoryImpl::~LedgerRepositoryFactoryImpl() {}

void LedgerRepositoryFactoryImpl::GetRepository(
    const fidl::String& repository_path,
    FirebaseConfigPtr firebase_config,
    fidl::InterfaceHandle<modular::auth::TokenProvider> token_provider,
    fidl::InterfaceRequest<LedgerRepository> repository_request,
    const GetRepositoryCallback& callback) {
  TRACE_DURATION("ledger", "repository_factory_get_repository");
  RepositoryInformation repository_information(repository_path);
  if (!repository_information.Init()) {
    callback(Status::IO_ERROR);
    return;
  }
  auto it = repositories_.find(repository_information.name);
  if (it != repositories_.end()) {
    it->second.BindRepository(std::move(repository_request),
                              std::move(callback));
    return;
  }

  if (!firebase_config || !token_provider) {
    FTL_LOG(WARNING) << "No sync configuration - Ledger will work locally but "
                     << "not sync. (running in Guest mode?)";

    auto ret = repositories_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(repository_information.name),
        std::forward_as_tuple(nullptr));
    LedgerRepositoryContainer* container = &ret.first->second;
    container->BindRepository(std::move(repository_request),
                              std::move(callback));
    std::unique_ptr<SyncWatcherSet> watchers =
        std::make_unique<SyncWatcherSet>();
    auto repository = std::make_unique<LedgerRepositoryImpl>(
        repository_path, environment_, std::move(watchers), nullptr);
    container->SetRepository(Status::OK, std::move(repository));
    return;
  }

  auto token_provider_ptr =
      modular::auth::TokenProviderPtr::Create(std::move(token_provider));
  token_provider_ptr.set_connection_error_handler(
      [ this, name = repository_information.name ] {
        FTL_LOG(ERROR) << "Lost connection to TokenProvider, "
                       << "shutting down the repository.";
        auto find_repository = repositories_.find(name);
        FTL_DCHECK(find_repository != repositories_.end());
        repositories_.erase(find_repository);
      });

  auto auth_provider = std::make_unique<AuthProviderImpl>(
      environment_->main_runner(), firebase_config->api_key,
      std::move(token_provider_ptr),
      std::make_unique<backoff::ExponentialBackoff>());
  cloud_sync::AuthProvider* auth_provider_ptr = auth_provider.get();

  auto ret =
      repositories_.emplace(std::piecewise_construct,
                            std::forward_as_tuple(repository_information.name),
                            std::forward_as_tuple(std::move(auth_provider)));
  LedgerRepositoryContainer* container = &ret.first->second;
  container->BindRepository(std::move(repository_request), std::move(callback));

  auto request = auth_provider_ptr->GetFirebaseUserId(ftl::MakeCopyable([
    this, repository_information, firebase_config = std::move(firebase_config),
    auth_provider_ptr, container
  ](cloud_sync::AuthStatus auth_status, std::string user_id) {
    if (auth_status != cloud_sync::AuthStatus::OK) {
      FTL_LOG(ERROR) << "Failed to retrieve Firebase user ID from the token "
                     << " manager, shutting down the repository.";
      container->SetRepository(Status::AUTHENTICATION_ERROR, nullptr);
      return;
    }

    cloud_sync::UserConfig user_config =
        GetUserConfig(environment_, firebase_config, user_id,
                      repository_information.content_path, auth_provider_ptr);
    CreateRepository(container, repository_information, std::move(user_config));

  }));
  auth_provider_requests_.emplace(std::move(request));
}

void LedgerRepositoryFactoryImpl::EraseRepository(
    const fidl::String& repository_path,
    FirebaseConfigPtr firebase_config,
    fidl::InterfaceHandle<modular::auth::TokenProvider> token_provider,
    const EraseRepositoryCallback& callback) {
  RepositoryInformation repository_information(repository_path);
  if (!repository_information.Init()) {
    callback(Status::IO_ERROR);
    return;
  }
  auto find_repository = repositories_.find(repository_information.name);
  if (find_repository != repositories_.end()) {
    FTL_LOG(WARNING) << "The repository to be erased is running, "
                     << "shutting it down before erasing.";
    find_repository->second.Detach();
  }

  Status status = DeleteRepositoryDirectory(repository_information);
  if (status != Status::OK) {
    callback(status);
    return;
  }

  if (!firebase_config || !token_provider) {
    // No sync configuration passed, only delete the local state.
    callback(Status::OK);
    return;
  }

  auto token_provider_ptr =
      modular::auth::TokenProviderPtr::Create(std::move(token_provider));

  delegate_->EraseRepository(
      EraseRemoteRepositoryOperation(
          environment_->main_runner(), environment_->network_service(),
          firebase_config->server_id, firebase_config->api_key,
          std::move(token_provider_ptr)),
      ftl::MakeCopyable([
        this, callback, delete_repository = std::move(find_repository)
      ](bool succeeded) {
        if (delete_repository != repositories_.end()) {
          repositories_.erase(delete_repository);
        }
        if (succeeded) {
          callback(Status::OK);
        } else {
          callback(Status::INTERNAL_ERROR);
        }
      }));
}

// Verifies that the current server id is not different from the server id used
// in a previous run and wipes the local state in case of a mismatch.
//
// Ledger does not support cloud migrations - once the repository is synced with
// a cloud, we can't change the server.
bool LedgerRepositoryFactoryImpl::CheckSyncConfig(
    const cloud_sync::UserConfig& user_config,
    const RepositoryInformation& repository_information) {
  FTL_DCHECK(user_config.use_sync);

  std::string server_id_path = ftl::Concatenate(
      {repository_information.content_path, "/", kServerIdFilename});
  if (files::IsFile(server_id_path)) {
    std::string previous_server_id;
    if (!files::ReadFileToString(server_id_path, &previous_server_id)) {
      FTL_LOG(ERROR) << "Failed to read the previous server id for "
                     << "compatibility check";
      return false;
    }

    if (previous_server_id == user_config.server_id) {
      return true;
    }

    FTL_LOG(WARNING) << "Mismatch between the previous server id: "
                     << previous_server_id
                     << " and the current one: " << user_config.server_id
                     << ".";
    FTL_LOG(WARNING) << "Ledger does not support cloud migrations: "
                     << "Deleting local state at "
                     << repository_information.content_path << ".";
    if (DeleteRepositoryDirectory(repository_information) != Status::OK) {
      FTL_LOG(ERROR) << "Unable to delete ledger directory. "
                     << "Reset Ledger using "
                     << "`rm -rf " << repository_information.content_path
                     << "`";
      return false;
    }
  }

  if (!files::WriteFileInTwoPhases(server_id_path, user_config.server_id,
                                   repository_information.staging_path)) {
    FTL_LOG(ERROR) << "Failed to write the current server_id for compatibility "
                   << "check.";
    return false;
  }

  return true;
}

void LedgerRepositoryFactoryImpl::CreateRepository(
    LedgerRepositoryContainer* container,
    const RepositoryInformation& repository_information,
    cloud_sync::UserConfig user_config) {
  const std::string temp_dir =
      ftl::Concatenate({repository_information.content_path, "/tmp"});
  if (config_persistence_ == ConfigPersistence::PERSIST &&
      !CheckSyncConfig(user_config, repository_information)) {
    container->SetRepository(Status::CONFIGURATION_ERROR, nullptr);
    return;
  }
  // Save debugging data for `ledger_tool`.
  if (config_persistence_ == ConfigPersistence::PERSIST &&
      !SaveConfigForDebugging(user_config.user_id,
                              repository_information.content_path, temp_dir)) {
    FTL_LOG(WARNING) << "Failed to save the current configuration.";
  }
  std::unique_ptr<SyncWatcherSet> watchers = std::make_unique<SyncWatcherSet>();
  ftl::Closure on_version_mismatch = [this, repository_information]() mutable {
    OnVersionMismatch(std::move(repository_information));
  };
  auto user_sync = std::make_unique<cloud_sync::UserSyncImpl>(
      environment_, std::move(user_config),
      std::make_unique<backoff::ExponentialBackoff>(), watchers.get(),
      std::move(on_version_mismatch));
  user_sync->Start();
  auto repository = std::make_unique<LedgerRepositoryImpl>(
      repository_information.content_path, environment_, std::move(watchers),
      std::move(user_sync));
  container->SetRepository(Status::OK, std::move(repository));
}

void LedgerRepositoryFactoryImpl::OnVersionMismatch(
    RepositoryInformation repository_information) {
  FTL_LOG(WARNING)
      << "Data in the cloud was wiped out, erasing local state. "
      << "This should log you out, log back in to start syncing again.";

  // First, shut down the repository so that we can delete the files while it's
  // not running.
  auto find_repository = repositories_.find(repository_information.name);
  FTL_DCHECK(find_repository != repositories_.end());
  find_repository->second.Detach();
  DeleteRepositoryDirectory(repository_information);
  repositories_.erase(find_repository);
}

Status LedgerRepositoryFactoryImpl::DeleteRepositoryDirectory(
    const RepositoryInformation& repository_information) {
  files::ScopedTempDir tmp_directory(repository_information.staging_path);
  std::string destination = tmp_directory.path() + "/content";
  if (rename(repository_information.content_path.c_str(),
             destination.c_str()) != 0) {
    FTL_LOG(ERROR) << "Unable to move repository local storage at "
                   << repository_information.content_path << " to "
                   << destination << ". Error: " << strerror(errno);
    return Status::IO_ERROR;
  }
  if (!files::DeletePath(destination, true)) {
    FTL_LOG(ERROR) << "Unable to delete repository staging storage at "
                   << destination;
    return Status::IO_ERROR;
  }
  return Status::OK;
}

}  // namespace ledger
