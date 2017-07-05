// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/merging/common_ancestor.h"

#include <algorithm>
#include <string>

#include "apps/ledger/src/app/constants.h"
#include "apps/ledger/src/app/merging/last_one_wins_merge_strategy.h"
#include "apps/ledger/src/app/merging/test_utils.h"
#include "apps/ledger/src/callback/cancellable_helper.h"
#include "apps/ledger/src/callback/capture.h"
#include "apps/ledger/src/glue/crypto/hash.h"
#include "apps/ledger/src/storage/public/constants.h"
#include "apps/ledger/src/storage/public/page_storage.h"
#include "apps/ledger/src/test/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/ftl/macros.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {
namespace {
class CommonAncestorTest : public test::TestWithPageStorage {
 public:
  CommonAncestorTest() {}
  ~CommonAncestorTest() override {}

 protected:
  storage::PageStorage* page_storage() override { return storage_.get(); }

  void SetUp() override {
    ::testing::Test::SetUp();
    ASSERT_TRUE(CreatePageStorage(&storage_));
  }

  std::unique_ptr<const storage::Commit> CreateCommit(
      storage::CommitIdView parent_id,
      std::function<void(storage::Journal*)> contents) {
    std::unique_ptr<storage::Journal> journal;
    EXPECT_EQ(storage::Status::OK,
              storage_->StartCommit(parent_id.ToString(),
                                    storage::JournalType::IMPLICIT, &journal));
    contents(journal.get());
    storage::Status actual_status;
    std::unique_ptr<const storage::Commit> actual_commit;
    storage_->CommitJournal(
        std::move(journal),
        callback::Capture(MakeQuitTask(), &actual_status, &actual_commit));
    EXPECT_FALSE(RunLoopWithTimeout());
    EXPECT_EQ(storage::Status::OK, actual_status);
    return actual_commit;
  }

  std::unique_ptr<const storage::Commit> CreateMergeCommit(
      storage::CommitIdView left,
      storage::CommitIdView right,
      std::function<void(storage::Journal*)> contents) {
    std::unique_ptr<storage::Journal> journal;
    EXPECT_EQ(storage::Status::OK,
              storage_->StartMergeCommit(left.ToString(), right.ToString(),
                                         &journal));
    contents(journal.get());
    storage::Status actual_status;
    std::unique_ptr<const storage::Commit> actual_commit;
    storage_->CommitJournal(
        std::move(journal),
        callback::Capture(MakeQuitTask(), &actual_status, &actual_commit));
    EXPECT_FALSE(RunLoopWithTimeout());
    EXPECT_EQ(storage::Status::OK, actual_status);
    return actual_commit;
  }

  std::unique_ptr<const storage::Commit> GetRoot() {
    storage::Status status;
    std::unique_ptr<const storage::Commit> root;
    storage_->GetCommit(storage::kFirstPageCommitId,
                        callback::Capture(MakeQuitTask(), &status, &root));
    EXPECT_FALSE(RunLoopWithTimeout());
    EXPECT_EQ(storage::Status::OK, status);
    return root;
  }

  std::unique_ptr<storage::PageStorage> storage_;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(CommonAncestorTest);
};

TEST_F(CommonAncestorTest, TwoChildrenOfRoot) {
  std::unique_ptr<const storage::Commit> commit_1 = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key", "a"));
  std::unique_ptr<const storage::Commit> commit_2 = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key", "b"));

  Status status;
  std::unique_ptr<const storage::Commit> result;
  FindCommonAncestor(message_loop_.task_runner(), storage_.get(),
                     std::move(commit_1), std::move(commit_2),
                     callback::Capture(MakeQuitTask(), &status, &result));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(storage::kFirstPageCommitId, result->GetId());
}

TEST_F(CommonAncestorTest, RootAndChild) {
  std::unique_ptr<const storage::Commit> root = GetRoot();

  std::unique_ptr<const storage::Commit> child = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key", "a"));

  Status status;
  std::unique_ptr<const storage::Commit> result;
  FindCommonAncestor(message_loop_.task_runner(), storage_.get(),
                     std::move(root), std::move(child),
                     callback::Capture(MakeQuitTask(), &status, &result));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(storage::kFirstPageCommitId, result->GetId());
}

// In this test the commits have the following structure:
//            (root)
//              /  \
//            (A)  (B)
//           /  \  /   \
//         (1) (merge) (2)
TEST_F(CommonAncestorTest, MergeCommitAndSomeOthers) {
  std::unique_ptr<const storage::Commit> commit_a = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key", "a"));
  std::unique_ptr<const storage::Commit> commit_b = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key", "b"));

  std::unique_ptr<const storage::Commit> commit_merge = CreateMergeCommit(
      commit_a->GetId(), commit_b->GetId(), AddKeyValueToJournal("key", "c"));

  std::unique_ptr<const storage::Commit> commit_1 =
      CreateCommit(commit_a->GetId(), AddKeyValueToJournal("key", "1"));
  std::unique_ptr<const storage::Commit> commit_2 =
      CreateCommit(commit_b->GetId(), AddKeyValueToJournal("key", "2"));

  // Ancestor of (1) and (merge) needs to be (root).
  Status status;
  std::unique_ptr<const storage::Commit> result;
  FindCommonAncestor(message_loop_.task_runner(), storage_.get(),
                     std::move(commit_1), std::move(commit_merge),
                     callback::Capture(MakeQuitTask(), &status, &result));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(storage::kFirstPageCommitId, result->GetId());

  // Ancestor of (2) and (A).
  FindCommonAncestor(message_loop_.task_runner(), storage_.get(),
                     std::move(commit_2), std::move(commit_a),
                     callback::Capture(MakeQuitTask(), &status, &result));
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(storage::kFirstPageCommitId, result->GetId());
}

// Regression test for LE-187.
TEST_F(CommonAncestorTest, LongChain) {
  const int length = 180;

  std::unique_ptr<const storage::Commit> commit_a = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key", "a"));
  std::unique_ptr<const storage::Commit> commit_b = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key", "b"));

  std::unique_ptr<const storage::Commit> last_commit = std::move(commit_a);
  for (int i = 0; i < length; i++) {
    last_commit = CreateCommit(last_commit->GetId(),
                               AddKeyValueToJournal(std::to_string(i), "val"));
  }

  // Ancestor of (last commit) and (b) needs to be (root).
  Status status;
  std::unique_ptr<const storage::Commit> result;
  FindCommonAncestor(message_loop_.task_runner(), storage_.get(),
                     std::move(last_commit), std::move(commit_b),
                     callback::Capture(MakeQuitTask(), &status, &result));
  // This test lasts ~2.5s on x86+qemu+kvm.
  EXPECT_FALSE(RunLoopWithTimeout(ftl::TimeDelta::FromSeconds(10)));
  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(storage::kFirstPageCommitId, result->GetId());
}

}  // namespace
}  // namespace ledger
