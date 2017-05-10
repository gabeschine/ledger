// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_IMPL_DB_IMPL_H_
#define APPS_LEDGER_SRC_STORAGE_IMPL_DB_IMPL_H_

#include <utility>

#include "apps/ledger/src/coroutine/coroutine.h"
#include "apps/ledger/src/storage/impl/db.h"

#include "leveldb/db.h"
#include "leveldb/write_batch.h"

namespace storage {

class PageStorageImpl;

class DbImpl : public DB {
 public:
  DbImpl(coroutine::CoroutineService* coroutine_service,
         PageStorageImpl* page_storage,
         std::string db_path);
  ~DbImpl() override;

  Status Init() override;
  std::unique_ptr<Batch> StartBatch() override;
  Status GetHeads(std::vector<CommitId>* heads) override;
  Status AddHead(CommitIdView head, int64_t timestamp) override;
  Status RemoveHead(CommitIdView head) override;
  Status GetCommitStorageBytes(CommitIdView commit_id,
                               std::string* storage_bytes) override;
  Status AddCommitStorageBytes(const CommitId& commit_id,
                               ftl::StringView storage_bytes) override;
  Status RemoveCommit(const CommitId& commit_id) override;
  Status CreateJournal(JournalType journal_type,
                       const CommitId& base,
                       std::unique_ptr<Journal>* journal) override;
  Status CreateMergeJournal(const CommitId& base,
                            const CommitId& other,
                            std::unique_ptr<Journal>* journal) override;
  Status GetImplicitJournalIds(std::vector<JournalId>* journal_ids) override;
  Status GetImplicitJournal(const JournalId& journal_id,
                            std::unique_ptr<Journal>* journal) override;
  Status RemoveExplicitJournals() override;
  Status RemoveJournal(const JournalId& journal_id) override;
  Status AddJournalEntry(const JournalId& journal_id,
                         ftl::StringView key,
                         ftl::StringView value,
                         KeyPriority priority) override;
  Status GetJournalValue(const JournalId& journal_id,
                         ftl::StringView key,
                         std::string* value) override;
  Status RemoveJournalEntry(const JournalId& journal_id,
                            convert::ExtendedStringView key) override;
  Status GetJournalValueCounter(const JournalId& journal_id,
                                ftl::StringView value,
                                int64_t* counter) override;
  Status SetJournalValueCounter(const JournalId& journal_id,
                                ftl::StringView value,
                                int64_t counter) override;
  Status GetJournalValues(const JournalId& journal_id,
                          std::vector<std::string>* values) override;
  Status GetJournalEntries(
      const JournalId& journal_id,
      std::unique_ptr<Iterator<const EntryChange>>* entries) override;
  Status GetUnsyncedCommitIds(std::vector<CommitId>* commit_ids) override;
  Status MarkCommitIdSynced(const CommitId& commit_id) override;
  Status MarkCommitIdUnsynced(const CommitId& commit_id,
                              int64_t timestamp) override;
  Status IsCommitSynced(const CommitId& commit_id, bool* is_synced) override;
  Status GetUnsyncedObjectIds(std::vector<ObjectId>* object_ids) override;
  Status MarkObjectIdSynced(ObjectIdView object_id) override;
  Status MarkObjectIdUnsynced(ObjectIdView object_id) override;
  Status IsObjectSynced(ObjectIdView object_id, bool* is_synced) override;
  Status SetSyncMetadata(ftl::StringView sync_state) override;
  Status GetSyncMetadata(std::string* sync_state) override;

 private:
  Status GetByPrefix(const leveldb::Slice& prefix,
                     std::vector<std::string>* key_suffixes);
  Status GetEntriesByPrefix(
      const leveldb::Slice& prefix,
      std::vector<std::pair<std::string, std::string>>* key_value_pairs);
  Status DeleteByPrefix(const leveldb::Slice& prefix);
  Status Get(convert::ExtendedStringView key, std::string* value);
  Status Put(convert::ExtendedStringView key, ftl::StringView value);
  Status Delete(convert::ExtendedStringView key);

  coroutine::CoroutineService* const coroutine_service_;
  PageStorageImpl* const page_storage_;
  const std::string db_path_;
  std::unique_ptr<leveldb::DB> db_;

  const leveldb::WriteOptions write_options_;
  const leveldb::ReadOptions read_options_;

  std::unique_ptr<leveldb::WriteBatch> batch_;
};

}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_IMPL_DB_IMPL_H_
