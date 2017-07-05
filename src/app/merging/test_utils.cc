// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/merging/test_utils.h"

#include "apps/ledger/src/app/constants.h"
#include "apps/ledger/src/callback/capture.h"
#include "apps/ledger/src/glue/crypto/hash.h"
#include "apps/ledger/src/storage/impl/page_storage_impl.h"
#include "apps/ledger/src/storage/public/constants.h"
#include "gtest/gtest.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {
namespace test {
TestBackoff::TestBackoff(int* get_next_count)
    : get_next_count_(get_next_count) {}
TestBackoff::~TestBackoff() {}

ftl::TimeDelta TestBackoff::GetNext() {
  (*get_next_count_)++;
  return ftl::TimeDelta::FromSeconds(0);
}

void TestBackoff::Reset() {}

TestWithPageStorage::TestWithPageStorage() : TestWithMessageLoop(){};

TestWithPageStorage::~TestWithPageStorage() {}

std::function<void(storage::Journal*)>
TestWithPageStorage::AddKeyValueToJournal(const std::string& key,
                                          std::string value) {
  return [ this, key,
           value = std::move(value) ](storage::Journal * journal) mutable {
    storage::Status status;
    storage::ObjectId object_id;
    page_storage()->AddObjectFromLocal(
        storage::DataSource::Create(std::move(value)),
        callback::Capture(MakeQuitTask(), &status, &object_id));
    EXPECT_FALSE(RunLoopWithTimeout());
    EXPECT_EQ(storage::Status::OK, status);
    EXPECT_EQ(storage::Status::OK,
              journal->Put(key, object_id, storage::KeyPriority::EAGER));
  };
}

std::function<void(storage::Journal*)>
TestWithPageStorage::DeleteKeyFromJournal(const std::string& key) {
  return [key](storage::Journal* journal) {
    EXPECT_EQ(storage::Status::OK, journal->Delete(key));
  };
}

::testing::AssertionResult TestWithPageStorage::GetValue(
    storage::ObjectIdView id,
    std::string* value) {
  storage::Status status;
  std::unique_ptr<const storage::Object> object;
  page_storage()->GetObject(
      id, storage::PageStorage::Location::LOCAL,
      callback::Capture(MakeQuitTask(), &status, &object));
  if (RunLoopWithTimeout()) {
    return ::testing::AssertionFailure()
           << "PageStorage::GetObject didn't return...";
  }
  if (status != storage::Status::OK) {
    return ::testing::AssertionFailure()
           << "PageStorage::GetObject returned status: " << status;
  }

  ftl::StringView data;
  status = object->GetData(&data);
  if (status != storage::Status::OK) {
    return ::testing::AssertionFailure()
           << "Object::GetData returned status: " << status;
  }

  *value = data.ToString();
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult TestWithPageStorage::CreatePageStorage(
    std::unique_ptr<storage::PageStorage>* page_storage) {
  std::unique_ptr<storage::PageStorageImpl> local_page_storage =
      std::make_unique<storage::PageStorageImpl>(
          &coroutine_service_, tmp_dir_.path(), kRootPageId.ToString());
  storage::Status status;
  local_page_storage->Init(callback::Capture(MakeQuitTask(), &status));
  if (RunLoopWithTimeout()) {
    return ::testing::AssertionFailure()
           << "PageStorageImpl::Init didn't return...";
  }

  if (status != storage::Status::OK) {
    return ::testing::AssertionFailure()
           << "PageStorageImpl::Init returned status: " << status;
  }
  *page_storage = std::move(local_page_storage);
  return ::testing::AssertionSuccess();
}

}  // namespace test
}  // namespace ledger
