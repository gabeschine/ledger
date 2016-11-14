// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/test/commit_empty_impl.h"

#include "lib/ftl/logging.h"

namespace storage {
namespace test {

CommitId CommitEmptyImpl::GetId() const {
  FTL_NOTIMPLEMENTED();
  return "NOT_IMPLEMENTED";
}

std::vector<CommitId> CommitEmptyImpl::GetParentIds() const {
  FTL_NOTIMPLEMENTED();
  return {};
}

int64_t CommitEmptyImpl::GetTimestamp() const {
  FTL_NOTIMPLEMENTED();
  return 0;
}

std::unique_ptr<CommitContents> CommitEmptyImpl::GetContents() const {
  FTL_NOTIMPLEMENTED();
  return nullptr;
}

ObjectId CommitEmptyImpl::CommitEmptyImpl::GetRootId() const {
  FTL_NOTIMPLEMENTED();
  return "NOT_IMPLEMENTED";
}

std::string CommitEmptyImpl::GetStorageBytes() const {
  FTL_NOTIMPLEMENTED();
  return "NOT_IMPLEMENTED";
}

}  // namespace test
}  // namespace storage