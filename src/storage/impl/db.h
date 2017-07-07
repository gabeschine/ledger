// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_IMPL_DB_H_
#define APPS_LEDGER_SRC_STORAGE_IMPL_DB_H_

#include "apps/ledger/src/storage/public/iterator.h"
#include "apps/ledger/src/storage/public/object.h"
#include "apps/ledger/src/storage/public/types.h"

namespace storage {

class Db {
 public:
  class Batch {
   public:
    Batch() {}
    virtual ~Batch() {}

    virtual Status Execute() = 0;

   private:
    FTL_DISALLOW_COPY_AND_ASSIGN(Batch);
  };

  Db() {}
  virtual ~Db() {}

  // Starts a new batch. Only one batch can be active at a time. The batch
  // will be written when Execute is called on the returned object. The Db
  // object must outlive the batch object.
  virtual std::unique_ptr<Batch> StartBatch() = 0;

  // Returns true if a batch is in progress; false otherwise.
  virtual bool BatchStarted() = 0;

  // Inserts the given key-value pair in the database.
  virtual Status Put(convert::ExtendedStringView key,
                     ftl::StringView value) = 0;

  // Retrieves the value corresponding to |key|.
  virtual Status Get(convert::ExtendedStringView key, std::string* value) = 0;

  // Deletes the entry in the database with the given |key|.
  virtual Status Delete(convert::ExtendedStringView key) = 0;

  // Checks whether |key| is stored in this database.
  virtual Status HasKey(convert::ExtendedStringView key, bool* has_key) = 0;

  // Retrieves the value for the given |key| as an Object with the provided
  // |object_id|.
  virtual Status GetObject(convert::ExtendedStringView key,
                           ObjectId object_id,
                           std::unique_ptr<const Object>* object) = 0;

  // Retrieves all keys matching the given |prefix|. |key_suffixes| will be
  // updated to contain the suffixes of corresponding keys.
  virtual Status GetByPrefix(convert::ExtendedStringView prefix,
                             std::vector<std::string>* key_suffixes) = 0;

  // Retrieves all entries matching the given |prefix|. The keys of the
  // returned entries will be updated not to contain the |prefix|.
  virtual Status GetEntriesByPrefix(
      convert::ExtendedStringView prefix,
      std::vector<std::pair<std::string, std::string>>* entries) = 0;

  // Retrieves an entry iterator over the entries whose keys start with
  // |prefix|.
  virtual Status GetIteratorAtPrefix(
      convert::ExtendedStringView prefix,
      std::unique_ptr<Iterator<const std::pair<convert::ExtendedStringView,
                                               convert::ExtendedStringView>>>*
          iterator) = 0;

  // Deletes all entries whose keys match the given |prefix|.
  virtual Status DeleteByPrefix(convert::ExtendedStringView prefix) = 0;
};

}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_IMPL_DB_H_
