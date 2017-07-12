// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/page_manager.h"

#include <memory>

#include "apps/ledger/src/app/constants.h"
#include "apps/ledger/src/app/merging/merge_resolver.h"
#include "apps/ledger/src/backoff/exponential_backoff.h"
#include "apps/ledger/src/cloud_sync/public/ledger_sync.h"
#include "apps/ledger/src/cloud_sync/test/page_sync_empty_impl.h"
#include "apps/ledger/src/coroutine/coroutine_impl.h"
#include "apps/ledger/src/environment/environment.h"
#include "apps/ledger/src/storage/fake/fake_page_storage.h"
#include "apps/ledger/src/storage/public/page_storage.h"
#include "apps/ledger/src/storage/public/types.h"
#include "apps/ledger/src/storage/test/commit_empty_impl.h"
#include "apps/ledger/src/test/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/ftl/macros.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {
namespace {

std::unique_ptr<MergeResolver> GetDummyResolver(Environment* environment,
                                                storage::PageStorage* storage) {
  return std::make_unique<MergeResolver>(
      [] {}, environment, storage,
      std::make_unique<backoff::ExponentialBackoff>(
          ftl::TimeDelta::FromSeconds(0), 1u, ftl::TimeDelta::FromSeconds(0)));
}

class FakePageSync : public cloud_sync::test::PageSyncEmptyImpl {
 public:
  void Start() override { start_called = true; }

  void SetOnBacklogDownloaded(
      ftl::Closure on_backlog_downloaded_callback) override {
    this->on_backlog_downloaded_callback =
        std::move(on_backlog_downloaded_callback);
  }

  void SetOnIdle(ftl::Closure on_idle) override { this->on_idle = on_idle; }

  void SetSyncWatcher(cloud_sync::SyncStateWatcher* watcher) override {
    this->watcher = watcher;
  }

  bool start_called = false;
  cloud_sync::SyncStateWatcher* watcher = nullptr;
  ftl::Closure on_backlog_downloaded_callback;
  ftl::Closure on_idle;
};

class PageManagerTest : public test::TestWithMessageLoop {
 public:
  PageManagerTest()
      : environment_(mtl::MessageLoop::GetCurrent()->task_runner(), nullptr) {}
  ~PageManagerTest() override {}

 protected:
  // ApplicationTestBase:
  void SetUp() override {
    ::testing::Test::SetUp();
    page_id_ = storage::PageId(kPageIdSize, 'a');
  }

  storage::PageId page_id_;
  Environment environment_;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(PageManagerTest);
};

TEST_F(PageManagerTest, OnEmptyCallback) {
  bool on_empty_called = false;
  auto storage = std::make_unique<storage::fake::FakePageStorage>(page_id_);
  auto merger = GetDummyResolver(&environment_, storage.get());
  PageManager page_manager(&environment_, std::move(storage), nullptr,
                           std::move(merger));
  page_manager.set_on_empty([this, &on_empty_called] {
    on_empty_called = true;
    message_loop_.PostQuitTask();
  });

  EXPECT_FALSE(on_empty_called);
  Status status;
  PagePtr page1;
  PagePtr page2;
  page_manager.BindPage(page1.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);

  page_manager.BindPage(page2.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);
  page1.reset();
  page2.reset();
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(on_empty_called);

  on_empty_called = false;
  PagePtr page3;
  page_manager.BindPage(page3.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);
  page3.reset();
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(on_empty_called);

  on_empty_called = false;
  PageSnapshotPtr snapshot;
  page_manager.BindPageSnapshot(
      std::make_unique<const storage::test::CommitEmptyImpl>(),
      snapshot.NewRequest(), "");
  snapshot.reset();
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(on_empty_called);
}

TEST_F(PageManagerTest, DeletingPageManagerClosesConnections) {
  auto storage = std::make_unique<storage::fake::FakePageStorage>(page_id_);
  auto merger = GetDummyResolver(&environment_, storage.get());
  auto page_manager = std::make_unique<PageManager>(
      &environment_, std::move(storage), nullptr, std::move(merger));

  Status status;
  PagePtr page;
  page_manager->BindPage(page.NewRequest(),
                         callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);
  bool page_closed = false;
  page.set_connection_error_handler([this, &page_closed] {
    page_closed = true;
    message_loop_.PostQuitTask();
  });

  page_manager.reset();
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(page_closed);
}

TEST_F(PageManagerTest, OnEmptyCallbackWithWatcher) {
  bool on_empty_called = false;
  auto storage = std::make_unique<storage::fake::FakePageStorage>(page_id_);
  auto merger = GetDummyResolver(&environment_, storage.get());
  PageManager page_manager(&environment_, std::move(storage), nullptr,
                           std::move(merger));
  page_manager.set_on_empty([this, &on_empty_called] {
    on_empty_called = true;
    message_loop_.PostQuitTask();
  });

  EXPECT_FALSE(on_empty_called);
  Status status;
  PagePtr page1;
  PagePtr page2;
  page_manager.BindPage(page1.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);
  page_manager.BindPage(page2.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);
  page1->Put(convert::ToArray("key1"), convert::ToArray("value1"),
             [this](Status status) {
               EXPECT_EQ(Status::OK, status);
               message_loop_.PostQuitTask();
             });
  EXPECT_FALSE(RunLoopWithTimeout());

  PageWatcherPtr watcher;
  fidl::InterfaceRequest<PageWatcher> watcher_request = watcher.NewRequest();
  PageSnapshotPtr snapshot;
  page1->GetSnapshot(snapshot.NewRequest(), nullptr, std::move(watcher),
                     [this](Status status) {
                       EXPECT_EQ(Status::OK, status);
                       message_loop_.PostQuitTask();
                     });
  EXPECT_FALSE(RunLoopWithTimeout());

  page1.reset();
  page2.reset();
  snapshot.reset();
  EXPECT_TRUE(RunLoopWithTimeout());
  EXPECT_FALSE(on_empty_called);

  watcher_request.PassChannel();
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(on_empty_called);
}

TEST_F(PageManagerTest, DelayBindingUntilSyncBacklogDownloaded) {
  auto fake_page_sync = std::make_unique<FakePageSync>();
  auto fake_page_sync_ptr = fake_page_sync.get();
  auto page_sync_context = std::make_unique<cloud_sync::PageSyncContext>();
  page_sync_context->page_sync = std::move(fake_page_sync);
  auto storage = std::make_unique<storage::fake::FakePageStorage>(page_id_);
  auto merger = GetDummyResolver(&environment_, storage.get());

  EXPECT_EQ(nullptr, fake_page_sync_ptr->watcher);
  EXPECT_FALSE(fake_page_sync_ptr->start_called);
  EXPECT_FALSE(fake_page_sync_ptr->on_backlog_downloaded_callback);

  PageManager page_manager(&environment_, std::move(storage),
                           std::move(page_sync_context), std::move(merger));

  EXPECT_NE(nullptr, fake_page_sync_ptr->watcher);
  EXPECT_TRUE(fake_page_sync_ptr->start_called);
  EXPECT_TRUE(fake_page_sync_ptr->on_backlog_downloaded_callback);

  bool called = false;
  Status status;
  PagePtr page;
  page_manager.BindPage(page.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  // The page shouldn't be bound until sync backlog is downloaded.
  EXPECT_TRUE(RunLoopWithTimeout(ftl::TimeDelta::FromMilliseconds(200)));

  page->GetId([this, &called](fidl::Array<uint8_t> id) {
    called = true;
    message_loop_.PostQuitTask();
  });

  EXPECT_TRUE(RunLoopWithTimeout(ftl::TimeDelta::FromMilliseconds(200)));
  EXPECT_FALSE(called);

  fake_page_sync_ptr->on_backlog_downloaded_callback();

  // BindPage callback can now be executed.
  EXPECT_FALSE(RunLoopWithTimeout());
  // GetId callback should then be called.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(called);

  // Check that a second call on the same manager is not delayed.
  called = false;
  page.reset();
  page_manager.BindPage(page.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);
  page->GetId([this, &called](fidl::Array<uint8_t> id) {
    called = true;
    message_loop_.PostQuitTask();
  });
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(called);
}

TEST_F(PageManagerTest, DelayBindingUntilSyncTimeout) {
  auto fake_page_sync = std::make_unique<FakePageSync>();
  auto fake_page_sync_ptr = fake_page_sync.get();
  auto page_sync_context = std::make_unique<cloud_sync::PageSyncContext>();
  page_sync_context->page_sync = std::move(fake_page_sync);
  auto storage = std::make_unique<storage::fake::FakePageStorage>(page_id_);
  auto merger = GetDummyResolver(&environment_, storage.get());

  EXPECT_EQ(nullptr, fake_page_sync_ptr->watcher);
  EXPECT_FALSE(fake_page_sync_ptr->start_called);
  EXPECT_FALSE(fake_page_sync_ptr->on_backlog_downloaded_callback);

  PageManager page_manager(&environment_, std::move(storage),
                           std::move(page_sync_context), std::move(merger),
                           ftl::TimeDelta::FromSeconds(0));

  EXPECT_NE(nullptr, fake_page_sync_ptr->watcher);
  EXPECT_TRUE(fake_page_sync_ptr->start_called);
  EXPECT_TRUE(fake_page_sync_ptr->on_backlog_downloaded_callback);

  bool called = false;
  Status status;
  PagePtr page;
  page_manager.BindPage(page.NewRequest(),
                        callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(Status::OK, status);
  page->GetId([this, &called](fidl::Array<uint8_t> id) {
    called = true;
    message_loop_.PostQuitTask();
  });

  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(called);
}

TEST_F(PageManagerTest, ExitWhenSyncFinishes) {
  auto fake_page_sync = std::make_unique<FakePageSync>();
  auto fake_page_sync_ptr = fake_page_sync.get();
  auto page_sync_context = std::make_unique<cloud_sync::PageSyncContext>();
  page_sync_context->page_sync = std::move(fake_page_sync);
  auto storage = std::make_unique<storage::fake::FakePageStorage>(page_id_);
  auto merger = GetDummyResolver(&environment_, storage.get());

  EXPECT_EQ(nullptr, fake_page_sync_ptr->watcher);
  EXPECT_FALSE(fake_page_sync_ptr->start_called);
  EXPECT_FALSE(fake_page_sync_ptr->on_backlog_downloaded_callback);

  PageManager page_manager(&environment_, std::move(storage),
                           std::move(page_sync_context), std::move(merger),
                           ftl::TimeDelta::FromSeconds(0));

  EXPECT_NE(nullptr, fake_page_sync_ptr->watcher);

  bool called = false;

  page_manager.set_on_empty([this, &called] {
    called = true;
    message_loop_.PostQuitTask();
  });

  message_loop_.task_runner()->PostTask(
      [fake_page_sync_ptr] { fake_page_sync_ptr->on_idle(); });

  EXPECT_FALSE(RunLoopWithTimeout());
}

}  // namespace
}  // namespace ledger
