// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/benchmark/sync/sync.h"

#include <iostream>

#include "apps/ledger/benchmark/lib/get_ledger.h"
#include "apps/ledger/benchmark/lib/logging.h"
#include "apps/ledger/src/convert/convert.h"
#include "apps/tracing/lib/trace/event.h"
#include "apps/tracing/lib/trace/provider.h"
#include "lib/ftl/command_line.h"
#include "lib/ftl/files/directory.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/string_number_conversions.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/vmo/strings.h"

namespace {
constexpr ftl::StringView kStoragePath = "/data/benchmark/ledger/sync";
constexpr ftl::StringView kEntryCountFlag = "entry-count";
constexpr ftl::StringView kValueSizeFlag = "value-size";
constexpr ftl::StringView kRefsFlag = "refs";
constexpr ftl::StringView kServerIdFlag = "server-id";

constexpr ftl::StringView kRefsOnFlag = "on";
constexpr ftl::StringView kRefsOffFlag = "off";
constexpr ftl::StringView kRefsAutoFlag = "auto";

constexpr size_t kKeySize = 100;
constexpr size_t kMaxInlineDataSize = MX_CHANNEL_MAX_MSG_BYTES * 9 / 10;

void PrintUsage(const char* executable_name) {
  std::cout << "Usage: " << executable_name << " --" << kEntryCountFlag
            << "=<int> --" << kValueSizeFlag << "=<int> --" << kRefsFlag << "=("
            << kRefsOnFlag << "|" << kRefsOffFlag << "|" << kRefsAutoFlag
            << ") --" << kServerIdFlag << "=<string>" << std::endl;
}

}  // namespace

namespace benchmark {

SyncBenchmark::SyncBenchmark(size_t entry_count,
                             size_t value_size,
                             ReferenceStrategy reference_strategy,
                             std::string server_id)
    : application_context_(app::ApplicationContext::CreateFromStartupInfo()),
      entry_count_(entry_count),
      value_size_(value_size),
      reference_strategy_(reference_strategy),
      server_id_(std::move(server_id)),
      page_watcher_binding_(this),
      alpha_tmp_dir_(kStoragePath),
      beta_tmp_dir_(kStoragePath),
      token_provider_impl_("",
                           "sync_user",
                           "sync_user@google.com",
                           "client_id") {
  FTL_DCHECK(entry_count > 0);
  FTL_DCHECK(value_size > 0);
  tracing::InitializeTracer(application_context_.get(),
                            {"benchmark_ledger_sync"});
}

void SyncBenchmark::Run() {
  // Name of the storage directory currently identifies the user. Ensure the
  // most nested directory has the same name to make the ledgers sync.
  std::string alpha_path = alpha_tmp_dir_.path() + "/sync_user";
  bool ret = files::CreateDirectory(alpha_path);
  FTL_DCHECK(ret);

  std::string beta_path = beta_tmp_dir_.path() + "/sync_user";
  ret = files::CreateDirectory(beta_path);
  FTL_DCHECK(ret);

  ledger::LedgerPtr alpha = benchmark::GetLedger(
      application_context_.get(), &alpha_controller_, &token_provider_impl_,
      "sync", alpha_path, true, server_id_);
  ledger::LedgerPtr beta = benchmark::GetLedger(
      application_context_.get(), &beta_controller_, &token_provider_impl_,
      "sync", beta_path, true, server_id_);

  benchmark::GetPageEnsureInitialized(
      alpha.get(), nullptr,
      ftl::MakeCopyable([ this, beta = std::move(beta) ](ledger::PagePtr page,
                                                         auto id) {
        page_id_ = id.Clone();
        alpha_page_ = std::move(page);
        beta->GetPage(std::move(id), beta_page_.NewRequest(),
                      benchmark::QuitOnErrorCallback("GetPage"));

        ledger::PageSnapshotPtr snapshot;
        beta_page_->GetSnapshot(
            snapshot.NewRequest(), nullptr, page_watcher_binding_.NewBinding(),
            [this](ledger::Status status) {
              if (benchmark::QuitOnError(status, "GetSnapshot")) {
                return;
              }
              RunSingle(0);
            });
      }));
}

void SyncBenchmark::OnChange(ledger::PageChangePtr page_change,
                             ledger::ResultState result_state,
                             const OnChangeCallback& callback) {
  FTL_DCHECK(page_change->changes.size() == 1);
  FTL_DCHECK(result_state == ledger::ResultState::COMPLETED);
  size_t i = std::stoul(convert::ToString(page_change->changes[0]->key));
  TRACE_ASYNC_END("benchmark", "sync latency", i);
  RunSingle(i + 1);
  callback(nullptr);
}

void SyncBenchmark::RunSingle(size_t i) {
  if (i == entry_count_) {
    Backlog();
    return;
  }

  fidl::Array<uint8_t> key = generator_.MakeKey(i, kKeySize);
  fidl::Array<uint8_t> value = generator_.MakeValue(value_size_);
  TRACE_ASYNC_BEGIN("benchmark", "sync latency", i);
  if (reference_strategy_ != ReferenceStrategy::OFF &&
      (reference_strategy_ == ReferenceStrategy::ON ||
       value_size_ > kMaxInlineDataSize)) {
    mx::vmo vmo;
    if (!mtl::VmoFromString(convert::ToStringView(value), &vmo)) {
      benchmark::QuitOnError(ledger::Status::IO_ERROR, "mtl::VmoFromString");
      return;
    }
    alpha_page_->CreateReferenceFromVmo(
        std::move(vmo),
        ftl::MakeCopyable([ this, key = std::move(key) ](
            ledger::Status status, ledger::ReferencePtr reference) mutable {
          if (benchmark::QuitOnError(status, "Page::CreateReferenceFromVmo")) {
            return;
          }
          alpha_page_->PutReference(
              std::move(key), std::move(reference), ledger::Priority::EAGER,
              benchmark::QuitOnErrorCallback("PutReference"));
        }));
    return;
  }
  alpha_page_->Put(std::move(key), std::move(value),
                   benchmark::QuitOnErrorCallback("Put"));
}

void SyncBenchmark::Backlog() {
  std::string gamma_path = gamma_tmp_dir_.path() + "/sync_user";
  bool ret = files::CreateDirectory(gamma_path);
  FTL_DCHECK(ret);

  gamma_ = benchmark::GetLedger(application_context_.get(), &gamma_controller_,
                                &token_provider_impl_, "sync", gamma_path, true,
                                server_id_);
  TRACE_ASYNC_BEGIN("benchmark", "get and verify backlog", 0);
  gamma_->GetPage(page_id_.Clone(), gamma_page_.NewRequest(),
                  [this](ledger::Status status) {
                    if (benchmark::QuitOnError(status, "GetPage")) {
                      return;
                    }
                    VerifyBacklog();
                  });
}

void SyncBenchmark::VerifyBacklog() {
  ledger::PageSnapshotPtr snapshot;
  gamma_page_->GetSnapshot(snapshot.NewRequest(), nullptr, nullptr,
                           benchmark::QuitOnErrorCallback("GetSnapshot"));

  ledger::PageSnapshot* snapshot_ptr = snapshot.get();
  snapshot_ptr->GetEntries(
      nullptr, nullptr,
      ftl::MakeCopyable([ this, snapshot = std::move(snapshot) ](
          ledger::Status status, auto entries, auto next_token) {
        if (benchmark::QuitOnError(status, "GetEntries")) {
          return;
        }
        if (entries.size() == static_cast<size_t>(entry_count_)) {
          TRACE_ASYNC_END("benchmark", "get and verify backlog", 0);
        }
        // If the number of entries does not match, don't record the end of the
        // verify backlog even, which will fail the benchmark.
        ShutDown();
      }));
}

void SyncBenchmark::ShutDown() {
  alpha_controller_->Kill();
  alpha_controller_.WaitForIncomingResponseWithTimeout(
      ftl::TimeDelta::FromSeconds(5));
  beta_controller_->Kill();
  beta_controller_.WaitForIncomingResponseWithTimeout(
      ftl::TimeDelta::FromSeconds(5));
  gamma_controller_->Kill();
  gamma_controller_.WaitForIncomingResponseWithTimeout(
      ftl::TimeDelta::FromSeconds(5));
  mtl::MessageLoop::GetCurrent()->PostQuitTask();
}
}  // namespace benchmark

int main(int argc, const char** argv) {
  ftl::CommandLine command_line = ftl::CommandLineFromArgcArgv(argc, argv);

  std::string entry_count_str;
  size_t entry_count;
  std::string value_size_str;
  size_t value_size;
  std::string reference_strategy_str;
  std::string server_id;
  if (!command_line.GetOptionValue(kEntryCountFlag.ToString(),
                                   &entry_count_str) ||
      !ftl::StringToNumberWithError(entry_count_str, &entry_count) ||
      entry_count <= 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(),
                                   &value_size_str) ||
      !ftl::StringToNumberWithError(value_size_str, &value_size) ||
      value_size <= 0 ||
      !command_line.GetOptionValue(kRefsFlag.ToString(),
                                   &reference_strategy_str) ||
      !command_line.GetOptionValue(kServerIdFlag.ToString(), &server_id)) {
    PrintUsage(argv[0]);
    return -1;
  }

  benchmark::SyncBenchmark::ReferenceStrategy reference_strategy;
  if (reference_strategy_str == kRefsOnFlag) {
    reference_strategy = benchmark::SyncBenchmark::ReferenceStrategy::ON;
  } else if (reference_strategy_str == kRefsOffFlag) {
    reference_strategy = benchmark::SyncBenchmark::ReferenceStrategy::OFF;
  } else if (reference_strategy_str == kRefsAutoFlag) {
    reference_strategy = benchmark::SyncBenchmark::ReferenceStrategy::AUTO;
  } else {
    std::cerr << "Unknown option " << reference_strategy_str << " for "
              << kRefsFlag.ToString() << std::endl;
    PrintUsage(argv[0]);
    return -1;
  }

  mtl::MessageLoop loop;
  benchmark::SyncBenchmark app(entry_count, value_size, reference_strategy,
                               server_id);
  loop.task_runner()->PostTask([&app] { app.Run(); });
  loop.Run();
  return 0;
}
