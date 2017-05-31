// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/tool/doctor_command.h"

#include <iostream>

#include "apps/ledger/src/cloud_provider/impl/cloud_provider_impl.h"
#include "apps/ledger/src/cloud_sync/impl/paths.h"
#include "apps/ledger/src/firebase/firebase_impl.h"
#include "apps/ledger/src/gcs/cloud_storage_impl.h"
#include "apps/ledger/src/glue/crypto/rand.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/strings/concatenate.h"
#include "lib/ftl/strings/string_view.h"
#include "lib/ftl/time/time_delta.h"
#include "lib/ftl/time/time_point.h"
#include "lib/mtl/socket/strings.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/vmo/strings.h"

namespace tool {

namespace {
const char kDoctorAppId[] = "__ledger_doctor__";

constexpr ftl::StringView kIndexConfigurationHint =
    "A Firebase commit query failed. "
    "Database index configuration might be incorrect or out of date. "
    "Please refer to the User Guide for the recommended setup. ";

std::string RandomString() {
  return std::to_string(glue::RandUint64());
}

void what(ftl::StringView what) {
  std::cout << " > " << what << std::endl;
}

void ok(ftl::StringView message = "") {
  std::cout << "   [OK] ";
  if (message.size()) {
    std::cout << message;
  }
  std::cout << std::endl;
}

void ok(ftl::TimeDelta request_time) {
  std::cout << "   [OK] request time " << request_time.ToMilliseconds() << " ms"
            << std::endl;
}

void error(ftl::StringView message) {
  std::cout << "   [FAILED] ";
  if (message.size()) {
    std::cout << " " << message;
  }
  std::cout << std::endl;
}

void error(cloud_provider::Status status) {
  std::cout << "   [FAILED] with cloud provider status " << status << std::endl;
}

void hint(ftl::StringView hint) {
  std::cout << "   hint: " << hint << std::endl;
  std::cout
      << "   see also the User Guide at "
      << "https://fuchsia.googlesource.com/ledger/+/HEAD/docs/user_guide.md"
      << std::endl;
}

std::string FirebaseUrlFromId(const std::string& firebase_id) {
  return ftl::Concatenate({"https://", firebase_id, ".firebaseio.com/.json"});
}

}  // namespace

DoctorCommand::DoctorCommand(cloud_sync::UserConfig* user_config,
                             ledger::NetworkService* network_service)
    : user_config_(user_config), network_service_(network_service) {
  FTL_DCHECK(network_service_);
  FTL_DCHECK(!user_config->server_id.empty());

  std::string app_firebase_path =
      cloud_sync::GetFirebasePathForApp(user_config_->user_id, kDoctorAppId);
  firebase_ = std::make_unique<firebase::FirebaseImpl>(
      network_service_, user_config_->server_id,
      cloud_sync::GetFirebasePathForPage(app_firebase_path, RandomString()));
  std::string app_gcs_prefix =
      cloud_sync::GetGcsPrefixForApp(user_config_->user_id, kDoctorAppId);
  cloud_storage_ = std::make_unique<gcs::CloudStorageImpl>(
      mtl::MessageLoop::GetCurrent()->task_runner(), network_service_,
      user_config_->server_id,
      cloud_sync::GetGcsPrefixForPage(app_gcs_prefix, RandomString()));
  cloud_provider_ = std::make_unique<cloud_provider::CloudProviderImpl>(
      firebase_.get(), cloud_storage_.get());
}

DoctorCommand::~DoctorCommand() {}

void DoctorCommand::Start(ftl::Closure on_done) {
  std::cout << "Sync Checkup" << std::endl;
  on_done_ = std::move(on_done);
  CheckHttpConnectivity();
}

void DoctorCommand::OnRemoteCommits(std::vector<cloud_provider::Commit> commits,
                                    std::string timestamp) {
  if (on_remote_commit_) {
    for (auto& commit : commits) {
      on_remote_commit_(std::move(commit), std::move(timestamp));
    }
  }
}

void DoctorCommand::OnConnectionError() {
  if (on_error_) {
    on_error_("connection error");
  }
}

void DoctorCommand::OnMalformedNotification() {
  if (on_error_) {
    on_error_("malformed notification");
  }
}

void DoctorCommand::CheckHttpConnectivity() {
  what("http - fetch http://example.com");

  auto request = network_service_->Request(
      [] {
        auto url_request = network::URLRequest::New();
        url_request->url = "http://example.com";
        return url_request;
      },
      [ this, request_start =
                  ftl::TimePoint::Now() ](network::URLResponsePtr response) {
        if (response->status_code != 200 || response->error) {
          error("network error " + response->error->description.get() +
                ", status code " + std::to_string(response->status_code));
          hint(
              "It looks like your Fuchsia doesn't have connectivity to the "
              "internets outside. Make sure to follow the instructions in "
              "https://fuchsia.googlesource.com/netstack/+/master/README.md");
          on_done_();
          return;
        }

        ftl::TimeDelta delta = ftl::TimePoint::Now() - request_start;
        ok(delta);
        CheckHttpsConnectivity();
      });
}

void DoctorCommand::CheckHttpsConnectivity() {
  what("https - fetch https://example.com");

  auto request = network_service_->Request(
      [] {
        auto url_request = network::URLRequest::New();
        url_request->url = "https://example.com";
        return url_request;
      },
      [ this, request_start =
                  ftl::TimePoint::Now() ](network::URLResponsePtr response) {
        if (response->status_code != 200 || response->error) {
          error("network error " + response->error->description.get() +
                ", status code " + std::to_string(response->status_code));
          hint(
              "It looks like the http*s* request failed even though http seems "
              "to work. Please file a Userspace bug for the network stack.");
          on_done_();
          return;
        }

        ftl::TimeDelta delta = ftl::TimePoint::Now() - request_start;
        ok(delta);
        CheckObjects();
      });
}

void DoctorCommand::CheckObjects() {
  what("GCS - upload test object (1 MB)");
  std::string id = RandomString();
  std::string content = std::string(1'000'000, 'a');
  content[42] = 'b';
  content[content.size() - 42] = 'c';

  mx::vmo data;
  auto result = mtl::VmoFromString(content, &data);
  FTL_DCHECK(result);

  ftl::TimePoint request_start = ftl::TimePoint::Now();
  cloud_provider_->AddObject(
      "", id, std::move(data),
      [this, id, content, request_start](cloud_provider::Status status) {

        if (status != cloud_provider::Status::OK) {
          error(status);
          hint(ftl::Concatenate(
              {"It seems that we can't access Firebase Storage / GCS server. "
               "Please refer to the User Guide for the "
               "recommended Firebase Storage configuration."}));
          on_done_();
          return;
        }
        ftl::TimeDelta delta = ftl::TimePoint::Now() - request_start;
        ok(delta);
        CheckGetObject(id, content);
      });
}

void DoctorCommand::CheckGetObject(std::string id,
                                   std::string expected_content) {
  what("GCS - retrieve test object");
  cloud_provider_->GetObject("", id, [
    this, expected_content = std::move(expected_content),
    request_start = ftl::TimePoint::Now()
  ](cloud_provider::Status status, uint64_t size, mx::socket data) {
    if (status != cloud_provider::Status::OK) {
      error(status);
      on_done_();
      return;
    }

    if (size != expected_content.size()) {
      error("Wrong size of the retrieved object: " + std::to_string(size) +
            " instead of " + std::to_string(expected_content.size()));
      on_done_();
      return;
    }

    std::string retrieved_content;
    if (!mtl::BlockingCopyToString(std::move(data), &retrieved_content)) {
      error("Failed to read the object content.");
      on_done_();
      return;
    }

    if (retrieved_content != expected_content) {
      error("Wrong content of the retrieved object.");
      on_done_();
      return;
    }

    ftl::TimeDelta delta = ftl::TimePoint::Now() - request_start;
    ok(delta);
    CheckCommits();
  });
}

void DoctorCommand::CheckCommits() {
  what("Firebase - upload test commit");
  cloud_provider::Commit commit(RandomString(), RandomString(), {});
  ftl::TimePoint request_start = ftl::TimePoint::Now();
  std::vector<cloud_provider::Commit> commits;
  commits.push_back(commit.Clone());
  cloud_provider_->AddCommits(
      "", std::move(commits),
      ftl::MakeCopyable([ this, commit = commit.Clone(),
                          request_start ](cloud_provider::Status status) {
        if (status != cloud_provider::Status::OK) {
          error(status);
          hint(ftl::Concatenate(
              {"It seems that we can't access the Firebase instance. "
               "Please verify that you can access ",
               FirebaseUrlFromId(user_config_->server_id),
               " on your host machine. If not, refer to the User Guide for the "
               "recommended Firebase configuration."}));
          on_done_();
          return;
        }
        ftl::TimeDelta delta = ftl::TimePoint::Now() - request_start;
        ok(delta);
        CheckGetCommits(commit.Clone());
      }));
}

void DoctorCommand::CheckGetCommits(cloud_provider::Commit commit) {
  what("Firebase - retrieve all commits");
  ftl::TimePoint request_start = ftl::TimePoint::Now();
  cloud_provider_->GetCommits(
      "", "",
      ftl::MakeCopyable([ this, commit = std::move(commit), request_start ](
          cloud_provider::Status status,
          std::vector<cloud_provider::Record> records) {
        if (status != cloud_provider::Status::OK) {
          error(status);
          hint(kIndexConfigurationHint);
          on_done_();
          return;
        }

        if (records.size() != 1) {
          error("Wrong number of commits received: " +
                std::to_string(records.size()));
          on_done_();
          return;
        }

        ftl::TimeDelta delta = ftl::TimePoint::Now() - request_start;
        ok(delta);
        CheckGetCommitsByTimestamp(commit.Clone(), records.front().timestamp);
      }));
}

void DoctorCommand::CheckGetCommitsByTimestamp(
    cloud_provider::Commit expected_commit,
    std::string timestamp) {
  what("Firebase - retrieve commits by timestamp");
  cloud_provider_->GetCommits("", timestamp,
                              ftl::MakeCopyable([
                                this, commit = std::move(expected_commit),
                                request_start = ftl::TimePoint::Now()
                              ](cloud_provider::Status status,
                                std::vector<cloud_provider::Record> records) {
                                if (status != cloud_provider::Status::OK) {
                                  error(status);
                                  hint(kIndexConfigurationHint);
                                  on_done_();
                                  return;
                                }

                                ftl::TimeDelta delta =
                                    ftl::TimePoint::Now() - request_start;
                                ok(delta);
                                CheckWatchExistingCommits(commit.Clone());
                              }));
}

void DoctorCommand::CheckWatchExistingCommits(
    cloud_provider::Commit expected_commit) {
  what("Firebase - watch for existing commits");
  on_remote_commit_ =
      ftl::MakeCopyable([ this, expected_commit = std::move(expected_commit) ](
          cloud_provider::Commit commit, std::string timestamp) {
        on_error_ = nullptr;
        if (commit.id != expected_commit.id ||
            commit.content != expected_commit.content) {
          error("received a wrong commit");
          on_done_();
          on_remote_commit_ = nullptr;
          return;
        }
        on_remote_commit_ = nullptr;
        ok();
        CheckWatchNewCommits();
      });
  on_error_ = [this](ftl::StringView description) {
    on_remote_commit_ = nullptr;
    on_error_ = nullptr;
    error(description);
    on_done_();
  };
  cloud_provider_->WatchCommits("", "", this);
}

void DoctorCommand::CheckWatchNewCommits() {
  what("Firebase - watch for new commits");
  cloud_provider::Commit commit(RandomString(), RandomString(), {});
  on_remote_commit_ = ftl::MakeCopyable([
    this, expected_commit = commit.Clone(),
    request_start = ftl::TimePoint::Now()
  ](cloud_provider::Commit commit, std::string timestamp) {
    on_error_ = nullptr;
    if (commit.id != expected_commit.id ||
        commit.content != expected_commit.content) {
      error("received a wrong commit");
      on_done_();
      on_remote_commit_ = nullptr;
      return;
    }
    ftl::TimeDelta delta = ftl::TimePoint::Now() - request_start;
    on_remote_commit_ = nullptr;
    ok(delta);
    Done();
  });
  on_error_ = [this](ftl::StringView description) {
    on_remote_commit_ = nullptr;
    on_error_ = nullptr;
    error(description);
    on_done_();
  };

  std::vector<cloud_provider::Commit> commits;
  commits.push_back(commit.Clone());
  cloud_provider_->AddCommits("", std::move(commits), ftl::MakeCopyable([
                                this, expected_commit = commit.Clone()
                              ](cloud_provider::Status status) {
                                if (status != cloud_provider::Status::OK) {
                                  error(status);
                                  on_done_();
                                  return;
                                }
                              }));
}

void DoctorCommand::Done() {
  std::cout << "You're all set!" << std::endl;
  on_done_();
}

}  // namespace tool
