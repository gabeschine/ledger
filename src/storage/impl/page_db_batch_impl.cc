// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/page_db_batch_impl.h"

#include <memory>

#include "apps/ledger/src/storage/impl/db_serialization.h"
#include "apps/ledger/src/storage/impl/journal_db_impl.h"
#include "apps/ledger/src/storage/impl/number_serialization.h"
#include "lib/ftl/strings/concatenate.h"

namespace storage {

PageDbBatchImpl::PageDbBatchImpl(std::unique_ptr<Db::Batch> batch,
                                 PageDb* db,
                                 coroutine::CoroutineService* coroutine_service,
                                 PageStorageImpl* page_storage)
    : batch_(std::move(batch)),
      db_(db),
      coroutine_service_(coroutine_service),
      page_storage_(page_storage) {}

PageDbBatchImpl::~PageDbBatchImpl() {}

Status PageDbBatchImpl::AddHead(coroutine::CoroutineHandler* /*handler*/,
                                CommitIdView head,
                                int64_t timestamp) {
  return batch_->Put(HeadRow::GetKeyFor(head), SerializeNumber(timestamp));
}

Status PageDbBatchImpl::RemoveHead(coroutine::CoroutineHandler* /*handler*/,
                                   CommitIdView head) {
  return batch_->Delete(HeadRow::GetKeyFor(head));
}

Status PageDbBatchImpl::AddCommitStorageBytes(
    coroutine::CoroutineHandler* /*handler*/,
    const CommitId& commit_id,
    ftl::StringView storage_bytes) {
  return batch_->Put(CommitRow::GetKeyFor(commit_id), storage_bytes);
}

Status PageDbBatchImpl::RemoveCommit(coroutine::CoroutineHandler* /*handler*/,
                                     const CommitId& commit_id) {
  return batch_->Delete(CommitRow::GetKeyFor(commit_id));
}

Status PageDbBatchImpl::CreateJournal(coroutine::CoroutineHandler* /*handler*/,
                                      JournalType journal_type,
                                      const CommitId& base,
                                      std::unique_ptr<Journal>* journal) {
  JournalId id = JournalEntryRow::NewJournalId(journal_type);
  *journal = JournalDBImpl::Simple(journal_type, coroutine_service_,
                                   page_storage_, db_, id, base);
  if (journal_type == JournalType::IMPLICIT) {
    return batch_->Put(ImplicitJournalMetaRow::GetKeyFor(id), base);
  }
  return Status::OK;
}

Status PageDbBatchImpl::CreateMergeJournal(
    coroutine::CoroutineHandler* /*handler*/,
    const CommitId& base,
    const CommitId& other,
    std::unique_ptr<Journal>* journal) {
  *journal = JournalDBImpl::Merge(
      coroutine_service_, page_storage_, db_,
      JournalEntryRow::NewJournalId(JournalType::EXPLICIT), base, other);
  return Status::OK;
}

Status PageDbBatchImpl::RemoveExplicitJournals(
    coroutine::CoroutineHandler* /*handler*/) {
  static std::string kExplicitJournalPrefix =
      ftl::Concatenate({JournalEntryRow::kPrefix,
                        ftl::StringView(&JournalEntryRow::kImplicitPrefix, 1)});
  return batch_->DeleteByPrefix(kExplicitJournalPrefix);
}

Status PageDbBatchImpl::RemoveJournal(const JournalId& journal_id) {
  if (journal_id[0] == JournalEntryRow::kImplicitPrefix) {
    Status status =
        batch_->Delete(ImplicitJournalMetaRow::GetKeyFor(journal_id));
    if (status != Status::OK) {
      return status;
    }
  }
  return batch_->DeleteByPrefix(JournalEntryRow::GetPrefixFor(journal_id));
}

Status PageDbBatchImpl::AddJournalEntry(const JournalId& journal_id,
                                        ftl::StringView key,
                                        ftl::StringView value,
                                        KeyPriority priority) {
  return batch_->Put(JournalEntryRow::GetKeyFor(journal_id, key),
                     JournalEntryRow::GetValueFor(value, priority));
}

Status PageDbBatchImpl::RemoveJournalEntry(const JournalId& journal_id,
                                           convert::ExtendedStringView key) {
  return batch_->Put(JournalEntryRow::GetKeyFor(journal_id, key),
                     JournalEntryRow::kDeletePrefix);
}

Status PageDbBatchImpl::WriteObject(
    coroutine::CoroutineHandler* handler,
    ObjectIdView object_id,
    std::unique_ptr<DataSource::DataChunk> content,
    PageDbObjectStatus object_status) {
  FTL_DCHECK(object_status > PageDbObjectStatus::UNKNOWN);

  auto object_key = ObjectRow::GetKeyFor(object_id);
  bool has_key;
  Status status = db_->HasObject(object_id, &has_key);
  if (status != Status::OK) {
    return status;
  }
  if (has_key && object_status > PageDbObjectStatus::TRANSIENT) {
    return SetObjectStatus(handler, object_id, object_status);
  }

  batch_->Put(object_key, content->Get());
  switch (object_status) {
    case PageDbObjectStatus::UNKNOWN:
      FTL_NOTREACHED();
      break;
    case PageDbObjectStatus::TRANSIENT:
      batch_->Put(TransientObjectRow::GetKeyFor(object_id), "");
      break;
    case PageDbObjectStatus::LOCAL:
      batch_->Put(LocalObjectRow::GetKeyFor(object_id), "");
      break;
    case PageDbObjectStatus::SYNCED:
      // Nothing to do.
      break;
  }
  return Status::OK;
}

Status PageDbBatchImpl::DeleteObject(coroutine::CoroutineHandler* /*handler*/,
                                     ObjectIdView object_id) {
  batch_->Delete(ObjectRow::GetKeyFor(object_id));
  batch_->Delete(TransientObjectRow::GetKeyFor(object_id));
  batch_->Delete(LocalObjectRow::GetKeyFor(object_id));
  return Status::OK;
}

Status PageDbBatchImpl::SetObjectStatus(
    coroutine::CoroutineHandler* /*handler*/,
    ObjectIdView object_id,
    PageDbObjectStatus object_status) {
  FTL_DCHECK(object_status >= PageDbObjectStatus::LOCAL);
  FTL_DCHECK(CheckHasObject(object_id))
      << "Unknown object: " << convert::ToHex(object_id);

  auto transient_key = TransientObjectRow::GetKeyFor(object_id);
  auto local_key = LocalObjectRow::GetKeyFor(object_id);

  switch (object_status) {
    case PageDbObjectStatus::UNKNOWN:
    case PageDbObjectStatus::TRANSIENT: {
      FTL_NOTREACHED();
      break;
    }
    case PageDbObjectStatus::LOCAL: {
      PageDbObjectStatus previous_object_status;
      Status status = db_->GetObjectStatus(object_id, &previous_object_status);
      if (status != Status::OK) {
        return status;
      }
      if (previous_object_status == PageDbObjectStatus::TRANSIENT) {
        batch_->Delete(transient_key);
        batch_->Put(local_key, "");
      }
      break;
    }
    case PageDbObjectStatus::SYNCED: {
      batch_->Delete(local_key);
      batch_->Delete(transient_key);
      break;
    }
  }

  return Status::OK;
}

Status PageDbBatchImpl::MarkCommitIdSynced(const CommitId& commit_id) {
  return batch_->Delete(UnsyncedCommitRow::GetKeyFor(commit_id));
}

Status PageDbBatchImpl::MarkCommitIdUnsynced(const CommitId& commit_id,
                                             uint64_t generation) {
  return batch_->Put(UnsyncedCommitRow::GetKeyFor(commit_id),
                     SerializeNumber(generation));
}

Status PageDbBatchImpl::SetSyncMetadata(
    coroutine::CoroutineHandler* /*handler*/,
    ftl::StringView key,
    ftl::StringView value) {
  return batch_->Put(SyncMetadataRow::GetKeyFor(key), value);
}

Status PageDbBatchImpl::Execute() {
  return batch_->Execute();
}

bool PageDbBatchImpl::CheckHasObject(convert::ExtendedStringView key) {
  bool result;
  Status status = db_->HasObject(key, &result);
  if (status != Status::OK) {
    return false;
  }
  return result;
}

}  // namespace storage
