// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_APP_MERGING_TEST_UTILS_H_
#define APPS_LEDGER_SRC_APP_MERGING_TEST_UTILS_H_

#include <functional>
#include <memory>

#include "apps/ledger/src/backoff/backoff.h"
#include "apps/ledger/src/coroutine/coroutine_impl.h"
#include "apps/ledger/src/storage/public/journal.h"
#include "apps/ledger/src/storage/public/page_storage.h"
#include "apps/ledger/src/storage/public/types.h"
#include "apps/ledger/src/test/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/ftl/files/scoped_temp_dir.h"

namespace ledger {
namespace test {
// Dummy implementation of a backoff policy, which always returns zero backoff
// time..
class TestBackoff : public backoff::Backoff {
 public:
  explicit TestBackoff(int* get_next_count);
  ~TestBackoff() override;

  ftl::TimeDelta GetNext() override;

  void Reset() override;

  int* get_next_count_;
};

class TestWithPageStorage : public ::test::TestWithMessageLoop {
 public:
  TestWithPageStorage();
  virtual ~TestWithPageStorage();

 protected:
  virtual storage::PageStorage* page_storage() = 0;

  // Returns a function that, when executed, adds the provided key and object to
  // a journal.
  std::function<void(storage::Journal*)> AddKeyValueToJournal(
      const std::string& key,
      std::string value);

  // Returns a function that, when executed, deleted the provided key from a
  // journal.
  std::function<void(storage::Journal*)> DeleteKeyFromJournal(
      const std::string& key);

  ::testing::AssertionResult GetValue(storage::ObjectIdView id,
                                      std::string* value);

  ::testing::AssertionResult CreatePageStorage(
      std::unique_ptr<storage::PageStorage>* page_storage);

 private:
  files::ScopedTempDir tmp_dir_;
  coroutine::CoroutineServiceImpl coroutine_service_;
};

}  // namespace test
}  // namespace ledger

#endif  // APPS_LEDGER_SRC_APP_MERGING_TEST_UTILS_H_
