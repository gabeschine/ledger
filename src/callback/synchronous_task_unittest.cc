// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/callback/synchronous_task.h"

#include <algorithm>

#include "gtest/gtest.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/threading/create_thread.h"

TEST(SynchronousTaskTest, RunSynchronouslyOnOtherThread) {
  constexpr size_t nb_values = 1000;
  ftl::RefPtr<ftl::TaskRunner> task_runner;
  std::thread thread = mtl::CreateThread(&task_runner);

  bool values[nb_values];
  std::fill_n(values, nb_values, false);
  for (bool& value : values) {
    task_runner->PostTask([&value] { value = true; });
  }
  bool called = false;
  ASSERT_TRUE(callback::RunSynchronously(task_runner,
                                         [&called] { called = true; },
                                         ftl::TimeDelta::FromSeconds(1)));
  for (bool value : values) {
    EXPECT_TRUE(value);
  }
  EXPECT_TRUE(called);

  task_runner->PostTask([] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  thread.join();
}

TEST(SynchronousTaskTest, RunOnCurrentThreadTimeout) {
  mtl::MessageLoop loop;
  bool called = false;
  EXPECT_FALSE(callback::RunSynchronously(
      loop.task_runner(), [&called] { called = true; },
      ftl::TimeDelta::FromMilliseconds(100)));
  EXPECT_FALSE(called);
}

TEST(SynchronousTaskTest, RunOnDeletedMessageLoop) {
  ftl::RefPtr<ftl::TaskRunner> task_runner;
  {
    mtl::MessageLoop loop;
    task_runner = loop.task_runner();
  }
  bool called = false;
  EXPECT_FALSE(
      callback::RunSynchronously(task_runner, [&called] { called = true; },
                                 ftl::TimeDelta::FromMilliseconds(100)));
  EXPECT_FALSE(called);
}
