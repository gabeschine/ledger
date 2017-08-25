// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/leveldb.h"

#include <utility>

#include "apps/ledger/src/cobalt/cobalt.h"
#include "apps/ledger/src/storage/impl/object_impl.h"
#include "apps/tracing/lib/trace/event.h"
#include "lib/ftl/files/directory.h"
#include "lib/ftl/files/path.h"

namespace storage {

namespace {

Status ConvertStatus(leveldb::Status s) {
  if (s.IsNotFound()) {
    return Status::NOT_FOUND;
  }
  if (!s.ok()) {
    FTL_LOG(ERROR) << "LevelDB error: " << s.ToString();
    return Status::INTERNAL_IO_ERROR;
  }
  return Status::OK;
}

class BatchImpl : public Db::Batch {
 public:
  // Creates a new Batch based on a leveldb batch. Once |Execute| is called,
  // |callback| will be called with the same batch, ready to be written in
  // leveldb. If the destructor is called without a previous execution of the
  // batch, |callback| will be called with a |nullptr|.
  BatchImpl(
      std::unique_ptr<leveldb::WriteBatch> batch,
      leveldb::DB* db,
      std::function<Status(std::unique_ptr<leveldb::WriteBatch>)> callback)
      : batch_(std::move(batch)), db_(db), callback_(std::move(callback)) {}

  ~BatchImpl() override {
    if (batch_)
      callback_(nullptr);
  }

  Status Put(convert::ExtendedStringView key, ftl::StringView value) override {
    FTL_DCHECK(batch_);
    batch_->Put(key, convert::ToSlice(value));
    return Status::OK;
  }

  Status Delete(convert::ExtendedStringView key) override {
    FTL_DCHECK(batch_);
    batch_->Delete(key);
    return Status::OK;
  }

  Status DeleteByPrefix(convert::ExtendedStringView prefix) override {
    FTL_DCHECK(batch_);
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_options_));
    for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix);
         it->Next()) {
      batch_->Delete(it->key());
    }
    return ConvertStatus(it->status());
  }

  Status Execute() override {
    FTL_DCHECK(batch_);
    return callback_(std::move(batch_));
  }

 private:
  std::unique_ptr<leveldb::WriteBatch> batch_;

  const leveldb::ReadOptions read_options_;
  leveldb::DB* db_;

  std::function<Status(std::unique_ptr<leveldb::WriteBatch>)> callback_;
};

class RowIterator
    : public Iterator<const std::pair<convert::ExtendedStringView,
                                      convert::ExtendedStringView>> {
 public:
  RowIterator(std::unique_ptr<leveldb::Iterator> it, std::string prefix)
      : it_(std::move(it)), prefix_(std::move(prefix)) {
    PrepareEntry();
  }

  ~RowIterator() override {}

  Iterator<const std::pair<convert::ExtendedStringView,
                           convert::ExtendedStringView>>&
  Next() override {
    it_->Next();
    PrepareEntry();
    return *this;
  }

  bool Valid() const override {
    return it_->Valid() && it_->key().starts_with(prefix_);
  }

  Status GetStatus() const override {
    return it_->status().ok() ? Status::OK : Status::INTERNAL_IO_ERROR;
  }

  const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>&
  operator*() const override {
    return *(row_.get());
  }

  const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>*
  operator->() const override {
    return row_.get();
  }

 private:
  void PrepareEntry() {
    if (!Valid()) {
      row_.reset(nullptr);
      return;
    }
    row_ = std::make_unique<
        std::pair<convert::ExtendedStringView, convert::ExtendedStringView>>(
        it_->key(), it_->value());
  }

  std::unique_ptr<leveldb::Iterator> it_;
  const std::string prefix_;

  std::unique_ptr<
      std::pair<convert::ExtendedStringView, convert::ExtendedStringView>>
      row_;
};

}  // namespace

LevelDb::LevelDb(std::string db_path) : db_path_(std::move(db_path)) {}

LevelDb::~LevelDb() {
  FTL_DCHECK(!active_batches_count_)
      << "Not all LevelDb batches have been executed or rolled back.";
}

Status LevelDb::Init() {
  TRACE_DURATION("ledger", "leveldb_init");
  if (!files::CreateDirectory(db_path_)) {
    FTL_LOG(ERROR) << "Failed to create directory under " << db_path_;
    return Status::INTERNAL_IO_ERROR;
  }
  leveldb::DB* db = nullptr;
  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::Status status = leveldb::DB::Open(options, db_path_, &db);
  if (status.IsCorruption()) {
    FTL_LOG(ERROR) << "Ledger state corrupted at " << db_path_
                   << " with leveldb status: " << status.ToString();
    FTL_LOG(WARNING) << "Trying to recover by erasing the local state.";
    FTL_LOG(WARNING)
        << "***** ALL LOCAL CHANGES IN THIS PAGE WILL BE LOST *****";
    ledger::ReportEvent(ledger::CobaltEvent::LEDGER_LEVELDB_STATE_CORRUPTED);

    if (!files::DeletePath(db_path_, true)) {
      FTL_LOG(ERROR) << "Failed to delete corrupted ledger at " << db_path_;
      return Status::INTERNAL_IO_ERROR;
    }
    leveldb::Status status = leveldb::DB::Open(options, db_path_, &db);
    if (!status.ok()) {
      FTL_LOG(ERROR) << "Failed to create a new LevelDB at " << db_path_
                     << " with leveldb status: " << status.ToString();
      return Status::INTERNAL_IO_ERROR;
    }
  } else if (!status.ok()) {
    FTL_LOG(ERROR) << "Failed to open ledger at " << db_path_
                   << " with leveldb status: " << status.ToString();
    return Status::INTERNAL_IO_ERROR;
  }
  db_.reset(db);
  return Status::OK;
}

std::unique_ptr<Db::Batch> LevelDb::StartBatch() {
  auto db_batch = std::make_unique<leveldb::WriteBatch>();
  active_batches_count_++;
  return std::make_unique<BatchImpl>(
      std::move(db_batch), db_.get(),
      [this](std::unique_ptr<leveldb::WriteBatch> db_batch) {
        active_batches_count_--;
        if (db_batch) {
          leveldb::Status status = db_->Write(write_options_, db_batch.get());
          if (!status.ok()) {
            FTL_LOG(ERROR) << "Failed to execute batch with status: "
                           << status.ToString();
            return Status::INTERNAL_IO_ERROR;
          }
        }
        return Status::OK;
      });
}

Status LevelDb::Get(convert::ExtendedStringView key, std::string* value) {
  return ConvertStatus(db_->Get(read_options_, key, value));
}

Status LevelDb::HasKey(convert::ExtendedStringView key, bool* has_key) {
  std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(read_options_));
  iterator->Seek(key);

  *has_key = iterator->Valid() && iterator->key() == key;
  return Status::OK;
}

Status LevelDb::GetObject(convert::ExtendedStringView key,
                          ObjectId object_id,
                          std::unique_ptr<const Object>* object) {
  std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(read_options_));
  iterator->Seek(key);

  if (!iterator->Valid() || iterator->key() != key) {
    return Status::NOT_FOUND;
  }

  if (object) {
    *object = std::make_unique<LevelDBObject>(std::move(object_id),
                                              std::move(iterator));
  }
  return Status::OK;
}

Status LevelDb::GetByPrefix(convert::ExtendedStringView prefix,
                            std::vector<std::string>* key_suffixes) {
  std::vector<std::string> result;
  std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_options_));
  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix);
       it->Next()) {
    leveldb::Slice key = it->key();
    key.remove_prefix(prefix.size());
    result.push_back(key.ToString());
  }
  if (!it->status().ok()) {
    return ConvertStatus(it->status());
  }
  key_suffixes->swap(result);
  return Status::OK;
}

Status LevelDb::GetEntriesByPrefix(
    convert::ExtendedStringView prefix,
    std::vector<std::pair<std::string, std::string>>* entries) {
  std::vector<std::pair<std::string, std::string>> result;
  std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_options_));
  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix);
       it->Next()) {
    leveldb::Slice key = it->key();
    key.remove_prefix(prefix.size());
    result.emplace_back(key.ToString(), it->value().ToString());
  }
  if (!it->status().ok()) {
    return ConvertStatus(it->status());
  }
  entries->swap(result);
  return Status::OK;
}

Status LevelDb::GetIteratorAtPrefix(
    convert::ExtendedStringView prefix,
    std::unique_ptr<Iterator<const std::pair<convert::ExtendedStringView,
                                             convert::ExtendedStringView>>>*
        iterator) {
  std::unique_ptr<leveldb::Iterator> local_iterator(
      db_->NewIterator(read_options_));
  local_iterator->Seek(prefix);

  if (iterator) {
    std::unique_ptr<Iterator<const std::pair<convert::ExtendedStringView,
                                             convert::ExtendedStringView>>>
        row_iterator = std::make_unique<RowIterator>(std::move(local_iterator),
                                                     prefix.ToString());
    iterator->swap(row_iterator);
  }
  return Status::OK;
}

}  // namespace storage
