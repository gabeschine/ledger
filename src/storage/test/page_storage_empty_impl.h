// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_TEST_PAGE_STORAGE_EMPTY_IMPL_H_
#define APPS_LEDGER_SRC_STORAGE_TEST_PAGE_STORAGE_EMPTY_IMPL_H_

#include <functional>
#include <memory>
#include <vector>

#include "apps/ledger/src/storage/public/page_storage.h"

namespace storage {
namespace test {

// Empty implementaton of PageStorage. All methods do nothing and return dummy
// or empty responses.
class PageStorageEmptyImpl : public PageStorage {
 public:
  PageStorageEmptyImpl() = default;
  ~PageStorageEmptyImpl() override = default;

  // PageStorage:
  PageId GetId() override;

  void SetSyncDelegate(PageSyncDelegate* page_sync) override;

  Status GetHeadCommitIds(std::vector<CommitId>* commit_ids) override;

  void GetCommit(CommitIdView commit_id,
                 std::function<void(Status, std::unique_ptr<const Commit>)>
                     callback) override;

  void AddCommitsFromSync(std::vector<CommitIdAndBytes> ids_and_bytes,
                          std::function<void(Status)> callback) override;

  Status StartCommit(const CommitId& commit_id,
                     JournalType journal_type,
                     std::unique_ptr<Journal>* journal) override;

  Status StartMergeCommit(const CommitId& left,
                          const CommitId& right,
                          std::unique_ptr<Journal>* journal) override;

  Status AddCommitWatcher(CommitWatcher* watcher) override;

  Status RemoveCommitWatcher(CommitWatcher* watcher) override;

  void GetUnsyncedCommits(
      std::function<void(Status, std::vector<std::unique_ptr<const Commit>>)>
          callback) override;

  Status MarkCommitSynced(const CommitId& commit_id) override;

  Status GetDeltaObjects(const CommitId& commit_id,
                         std::vector<ObjectId>* objects) override;

  void GetUnsyncedObjectIds(
      const CommitId& commit_id,
      std::function<void(Status, std::vector<ObjectId>)> callback) override;

  Status MarkObjectSynced(ObjectIdView object_id) override;

  void AddObjectFromSync(ObjectIdView object_id,
                         std::unique_ptr<DataSource> data_source,
                         const std::function<void(Status)>& callback) override;

  void AddObjectFromLocal(
      std::unique_ptr<DataSource> data_source,
      const std::function<void(Status, ObjectId)>& callback) override;

  void GetObject(
      ObjectIdView object_id,
      Location location,
      const std::function<void(Status, std::unique_ptr<const Object>)>&
          callback) override;

  Status SetSyncMetadata(ftl::StringView key, ftl::StringView value) override;

  Status GetSyncMetadata(ftl::StringView key, std::string* value) override;

  void GetCommitContents(const Commit& commit,
                         std::string min_key,
                         std::function<bool(Entry)> on_next,
                         std::function<void(Status)> on_done) override;

  void GetEntryFromCommit(const Commit& commit,
                          std::string key,
                          std::function<void(Status, Entry)> callback) override;

  void GetCommitContentsDiff(const Commit& base_commit,
                             const Commit& other_commit,
                             std::string min_key,
                             std::function<bool(EntryChange)> on_next_diff,
                             std::function<void(Status)> on_done) override;
};

}  // namespace test
}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_TEST_PAGE_STORAGE_EMPTY_IMPL_H_
