// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CALLBACK_SYNCHRONOUS_TASK_H_
#define APPS_LEDGER_SRC_CALLBACK_SYNCHRONOUS_TASK_H_

#include "lib/ftl/functional/closure.h"
#include "lib/ftl/tasks/task_runner.h"
#include "lib/ftl/time/time_delta.h"

namespace callback {

// Posts |task| on |task_runner| and waits up to |timeout| for it to run.
// Returns |true| if the task has been run. The task can fail to run either
// because the message loop associated with |task_runner| is deleted, or because
// the calls timed out.
bool RunSynchronously(const ftl::RefPtr<ftl::TaskRunner>& task_runner,
                      ftl::Closure task,
                      ftl::TimeDelta timeout);
}  // namespace callback

#endif  // APPS_LEDGER_SRC_CALLBACK_SYNCHRONOUS_TASK_H_
