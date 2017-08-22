// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/page_db.h"

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "apps/ledger/src/coroutine/coroutine_impl.h"
#include "apps/ledger/src/glue/crypto/rand.h"
#include "apps/ledger/src/storage/impl/commit_impl.h"
#include "apps/ledger/src/storage/impl/commit_random_impl.h"
#include "apps/ledger/src/storage/impl/journal_db_impl.h"
#include "apps/ledger/src/storage/impl/page_db_impl.h"
#include "apps/ledger/src/storage/impl/page_storage_impl.h"
#include "apps/ledger/src/storage/impl/storage_test_utils.h"
#include "apps/ledger/src/storage/public/constants.h"
#include "apps/ledger/src/test/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/ftl/files/scoped_temp_dir.h"
#include "lib/ftl/macros.h"
#include "lib/mtl/tasks/message_loop.h"

namespace storage {
namespace {

void ExpectChangesEqual(const EntryChange& expected, const EntryChange& found) {
  EXPECT_EQ(expected.deleted, found.deleted);
  EXPECT_EQ(expected.entry.key, found.entry.key);
  if (!expected.deleted) {
    // If the entry is deleted, object_id and priority are not valid.
    EXPECT_EQ(expected.entry.object_id, found.entry.object_id);
    EXPECT_EQ(expected.entry.priority, found.entry.priority);
  }
}

class PageDbTest : public ::test::TestWithMessageLoop {
 public:
  PageDbTest()
      : page_storage_(&coroutine_service_, tmp_dir_.path(), "page_id"),
        page_db_(&coroutine_service_, &page_storage_, tmp_dir_.path()) {}

  ~PageDbTest() override {}

  // Test:
  void SetUp() override {
    std::srand(0);
    ASSERT_EQ(Status::OK, page_db_.Init());
    coroutine_service_.StartCoroutine(
        callback::Capture(MakeQuitTask(), &handler_));
    EXPECT_FALSE(RunLoopWithTimeout());
  }

 protected:
  coroutine::CoroutineHandler* handler_;
  files::ScopedTempDir tmp_dir_;
  coroutine::CoroutineServiceImpl coroutine_service_;
  PageStorageImpl page_storage_;
  PageDbImpl page_db_;

  FTL_DISALLOW_COPY_AND_ASSIGN(PageDbTest);
};

TEST_F(PageDbTest, HeadCommits) {
  std::vector<CommitId> heads;
  EXPECT_EQ(Status::OK, page_db_.GetHeads(&heads));
  EXPECT_TRUE(heads.empty());

  CommitId cid = RandomCommitId();
  EXPECT_EQ(Status::OK, page_db_.AddHead(cid, glue::RandUint64()));
  EXPECT_EQ(Status::OK, page_db_.GetHeads(&heads));
  EXPECT_EQ(1u, heads.size());
  EXPECT_EQ(cid, heads[0]);

  EXPECT_EQ(Status::OK, page_db_.RemoveHead(handler_, cid));
  EXPECT_EQ(Status::OK, page_db_.GetHeads(&heads));
  EXPECT_TRUE(heads.empty());
}

TEST_F(PageDbTest, OrderHeadCommitsByTimestamp) {
  std::vector<int64_t> timestamps = {std::numeric_limits<int64_t>::min(),
                                     std::numeric_limits<int64_t>::max(), 0};

  for (size_t i = 0; i < 10; ++i) {
    int64_t ts;
    do {
      ts = glue::RandUint64();
    } while (std::find(timestamps.begin(), timestamps.end(), ts) !=
             timestamps.end());
    timestamps.push_back(ts);
  }

  auto sorted_timestamps = timestamps;
  std::sort(sorted_timestamps.begin(), sorted_timestamps.end());
  auto random_ordered_timestamps = timestamps;
  auto rng = std::default_random_engine(42);
  std::shuffle(random_ordered_timestamps.begin(),
               random_ordered_timestamps.end(), rng);

  std::map<int64_t, CommitId> commits;
  for (auto ts : random_ordered_timestamps) {
    commits[ts] = RandomCommitId();
    EXPECT_EQ(Status::OK, page_db_.AddHead(commits[ts], ts));
  }

  std::vector<CommitId> heads;
  EXPECT_EQ(Status::OK, page_db_.GetHeads(&heads));
  EXPECT_EQ(timestamps.size(), heads.size());

  for (size_t i = 0; i < heads.size(); ++i) {
    EXPECT_EQ(commits[sorted_timestamps[i]], heads[i]);
  }
}

TEST_F(PageDbTest, Commits) {
  std::vector<std::unique_ptr<const Commit>> parents;
  parents.emplace_back(new test::CommitRandomImpl());

  std::unique_ptr<Commit> stored_commit;
  std::string storage_bytes;
  std::unique_ptr<Commit> commit = CommitImpl::FromContentAndParents(
      &page_storage_, RandomObjectId(), std::move(parents));

  EXPECT_EQ(Status::NOT_FOUND,
            page_db_.GetCommitStorageBytes(commit->GetId(), &storage_bytes));

  EXPECT_EQ(Status::OK,
            page_db_.AddCommitStorageBytes(handler_, commit->GetId(),
                                           commit->GetStorageBytes()));
  EXPECT_EQ(Status::OK,
            page_db_.GetCommitStorageBytes(commit->GetId(), &storage_bytes));
  EXPECT_EQ(storage_bytes, commit->GetStorageBytes());

  EXPECT_EQ(Status::OK, page_db_.RemoveCommit(handler_, commit->GetId()));
  EXPECT_EQ(Status::NOT_FOUND,
            page_db_.GetCommitStorageBytes(commit->GetId(), &storage_bytes));
}

TEST_F(PageDbTest, Journals) {
  CommitId commit_id = RandomCommitId();

  std::unique_ptr<Journal> implicit_journal;
  std::unique_ptr<Journal> explicit_journal;
  EXPECT_EQ(Status::OK, page_db_.CreateJournal(JournalType::IMPLICIT, commit_id,
                                               &implicit_journal));
  EXPECT_EQ(Status::OK, page_db_.CreateJournal(JournalType::EXPLICIT, commit_id,
                                               &explicit_journal));

  EXPECT_EQ(Status::OK, page_db_.RemoveExplicitJournals());

  // Removing explicit journals should not affect the implicit ones.
  std::vector<JournalId> journal_ids;
  EXPECT_EQ(Status::OK, page_db_.GetImplicitJournalIds(&journal_ids));
  EXPECT_EQ(1u, journal_ids.size());

  std::unique_ptr<Journal> found_journal;
  EXPECT_EQ(Status::OK,
            page_db_.GetImplicitJournal(journal_ids[0], &found_journal));
  EXPECT_EQ(Status::OK, page_db_.RemoveJournal(journal_ids[0]));
  EXPECT_EQ(Status::NOT_FOUND,
            page_db_.GetImplicitJournal(journal_ids[0], &found_journal));
  EXPECT_EQ(Status::OK, page_db_.GetImplicitJournalIds(&journal_ids));
  EXPECT_EQ(0u, journal_ids.size());
}

TEST_F(PageDbTest, JournalEntries) {
  CommitId commit_id = RandomCommitId();

  std::unique_ptr<Journal> implicit_journal;
  EXPECT_EQ(Status::OK, page_db_.CreateJournal(JournalType::IMPLICIT, commit_id,
                                               &implicit_journal));
  EXPECT_EQ(Status::OK,
            implicit_journal->Put("add-key-1", "value1", KeyPriority::LAZY));
  EXPECT_EQ(Status::OK,
            implicit_journal->Put("add-key-2", "value2", KeyPriority::EAGER));
  EXPECT_EQ(Status::OK,
            implicit_journal->Put("add-key-1", "value3", KeyPriority::LAZY));
  EXPECT_EQ(Status::OK, implicit_journal->Delete("remove-key"));

  EntryChange expected_changes[] = {
      NewEntryChange("add-key-1", "value3", KeyPriority::LAZY),
      NewEntryChange("add-key-2", "value2", KeyPriority::EAGER),
      NewRemoveEntryChange("remove-key"),
  };
  std::unique_ptr<Iterator<const EntryChange>> entries;
  EXPECT_EQ(Status::OK,
            page_db_.GetJournalEntries(
                static_cast<JournalDBImpl*>(implicit_journal.get())->GetId(),
                &entries));
  for (const auto& expected_change : expected_changes) {
    EXPECT_TRUE(entries->Valid());
    ExpectChangesEqual(expected_change, **entries);
    entries->Next();
  }
  EXPECT_FALSE(entries->Valid());
  EXPECT_EQ(Status::OK, entries->GetStatus());
}

TEST_F(PageDbTest, ObjectStorage) {
  ObjectId object_id = RandomObjectId();
  std::string content = RandomString(32 * 1024);
  std::unique_ptr<const Object> object;
  PageDbObjectStatus object_status;

  EXPECT_EQ(Status::NOT_FOUND, page_db_.ReadObject(object_id, &object));
  ASSERT_EQ(Status::OK,
            page_db_.WriteObject(handler_, object_id,
                                 DataSource::DataChunk::Create(content),
                                 PageDbObjectStatus::TRANSIENT));
  page_db_.GetObjectStatus(object_id, &object_status);
  EXPECT_EQ(PageDbObjectStatus::TRANSIENT, object_status);
  ASSERT_EQ(Status::OK, page_db_.ReadObject(object_id, &object));
  ftl::StringView object_content;
  EXPECT_EQ(Status::OK, object->GetData(&object_content));
  EXPECT_EQ(content, object_content);
  EXPECT_EQ(Status::OK, page_db_.DeleteObject(handler_, object_id));
  EXPECT_EQ(Status::NOT_FOUND, page_db_.ReadObject(object_id, &object));
}

TEST_F(PageDbTest, UnsyncedCommits) {
  CommitId commit_id = RandomCommitId();
  std::vector<CommitId> commit_ids;
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedCommitIds(&commit_ids));
  EXPECT_TRUE(commit_ids.empty());

  EXPECT_EQ(Status::OK, page_db_.MarkCommitIdUnsynced(commit_id, 0));
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedCommitIds(&commit_ids));
  EXPECT_EQ(1u, commit_ids.size());
  EXPECT_EQ(commit_id, commit_ids[0]);
  bool is_synced;
  EXPECT_EQ(Status::OK, page_db_.IsCommitSynced(commit_id, &is_synced));
  EXPECT_FALSE(is_synced);

  EXPECT_EQ(Status::OK, page_db_.MarkCommitIdSynced(commit_id));
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedCommitIds(&commit_ids));
  EXPECT_TRUE(commit_ids.empty());
  EXPECT_EQ(Status::OK, page_db_.IsCommitSynced(commit_id, &is_synced));
  EXPECT_TRUE(is_synced);
}

TEST_F(PageDbTest, OrderUnsyncedCommitsByTimestamp) {
  CommitId commit_ids[] = {RandomCommitId(), RandomCommitId(),
                           RandomCommitId()};
  // Add three unsynced commits with timestamps 200, 300 and 100.
  EXPECT_EQ(Status::OK, page_db_.MarkCommitIdUnsynced(commit_ids[0], 200));
  EXPECT_EQ(Status::OK, page_db_.MarkCommitIdUnsynced(commit_ids[1], 300));
  EXPECT_EQ(Status::OK, page_db_.MarkCommitIdUnsynced(commit_ids[2], 100));

  // The result should be ordered by the given timestamps.
  std::vector<CommitId> found_ids;
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedCommitIds(&found_ids));
  EXPECT_EQ(3u, found_ids.size());
  EXPECT_EQ(found_ids[0], commit_ids[2]);
  EXPECT_EQ(found_ids[1], commit_ids[0]);
  EXPECT_EQ(found_ids[2], commit_ids[1]);
}

TEST_F(PageDbTest, UnsyncedPieces) {
  ObjectId object_id = RandomObjectId();
  std::vector<ObjectId> object_ids;
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedPieces(&object_ids));
  EXPECT_TRUE(object_ids.empty());

  EXPECT_EQ(Status::OK, page_db_.WriteObject(handler_, object_id,
                                             DataSource::DataChunk::Create(""),
                                             PageDbObjectStatus::LOCAL));
  EXPECT_EQ(Status::OK,
            page_db_.SetObjectStatus(object_id, PageDbObjectStatus::LOCAL));
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedPieces(&object_ids));
  EXPECT_EQ(1u, object_ids.size());
  EXPECT_EQ(object_id, object_ids[0]);
  PageDbObjectStatus object_status;
  EXPECT_EQ(Status::OK, page_db_.GetObjectStatus(object_id, &object_status));
  EXPECT_EQ(PageDbObjectStatus::LOCAL, object_status);

  EXPECT_EQ(Status::OK,
            page_db_.SetObjectStatus(object_id, PageDbObjectStatus::SYNCED));
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedPieces(&object_ids));
  EXPECT_TRUE(object_ids.empty());
  EXPECT_EQ(Status::OK, page_db_.GetObjectStatus(object_id, &object_status));
  EXPECT_EQ(PageDbObjectStatus::SYNCED, object_status);
}

TEST_F(PageDbTest, Batch) {
  std::unique_ptr<PageDb::Batch> batch = page_db_.StartBatch();

  ObjectId object_id = RandomObjectId();
  EXPECT_EQ(Status::OK, batch->WriteObject(handler_, object_id,
                                           DataSource::DataChunk::Create(""),
                                           PageDbObjectStatus::LOCAL));

  std::vector<ObjectId> object_ids;
  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedPieces(&object_ids));
  EXPECT_TRUE(object_ids.empty());

  EXPECT_EQ(Status::OK, batch->Execute());

  EXPECT_EQ(Status::OK, page_db_.GetUnsyncedPieces(&object_ids));
  EXPECT_EQ(1u, object_ids.size());
  EXPECT_EQ(object_id, object_ids[0]);
}

TEST_F(PageDbTest, PageDbObjectStatus) {
  ObjectId object_id = RandomObjectId();
  PageDbObjectStatus object_status;

  ASSERT_EQ(Status::OK, page_db_.GetObjectStatus(object_id, &object_status));
  EXPECT_EQ(PageDbObjectStatus::UNKNOWN, object_status);

  PageDbObjectStatus initial_statuses[] = {PageDbObjectStatus::TRANSIENT,
                                           PageDbObjectStatus::LOCAL,
                                           PageDbObjectStatus::SYNCED};
  PageDbObjectStatus next_statuses[] = {PageDbObjectStatus::LOCAL,
                                        PageDbObjectStatus::SYNCED};
  for (auto initial_status : initial_statuses) {
    for (auto next_status : next_statuses) {
      ASSERT_EQ(Status::OK, page_db_.DeleteObject(handler_, object_id));
      ASSERT_EQ(Status::OK,
                page_db_.WriteObject(handler_, object_id,
                                     DataSource::DataChunk::Create(""),
                                     initial_status));
      ASSERT_EQ(Status::OK,
                page_db_.GetObjectStatus(object_id, &object_status));
      EXPECT_EQ(initial_status, object_status);
      ASSERT_EQ(Status::OK, page_db_.SetObjectStatus(object_id, next_status));

      PageDbObjectStatus expected_status =
          std::max(initial_status, next_status);
      ASSERT_EQ(Status::OK,
                page_db_.GetObjectStatus(object_id, &object_status));
      EXPECT_EQ(expected_status, object_status);
    }
  }
}

TEST_F(PageDbTest, SyncMetadata) {
  std::vector<std::pair<ftl::StringView, ftl::StringView>> keys_and_values = {
      {"foo1", "foo2"}, {"bar1", " bar2 "}};
  for (auto key_and_value : keys_and_values) {
    auto key = key_and_value.first;
    auto value = key_and_value.second;
    std::string returned_value;
    EXPECT_EQ(Status::NOT_FOUND,
              page_db_.GetSyncMetadata(key, &returned_value));

    EXPECT_EQ(Status::OK, page_db_.SetSyncMetadata(key, value));
    EXPECT_EQ(Status::OK, page_db_.GetSyncMetadata(key, &returned_value));
    EXPECT_EQ(value, returned_value);
  }
}

}  // namespace
}  // namespace storage
