// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/commit_impl.h"

#include <algorithm>

#include <flatbuffers/flatbuffers.h>
#include <sys/time.h>

#include "apps/ledger/src/convert/convert.h"
#include "apps/ledger/src/glue/crypto/hash.h"
#include "apps/ledger/src/storage/impl/btree/tree_node.h"
#include "apps/ledger/src/storage/impl/commit_generated.h"
#include "apps/ledger/src/storage/public/constants.h"
#include "lib/ftl/build_config.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/memory/ref_counted.h"

namespace storage {

class CommitImpl::SharedStorageBytes
    : public ftl::RefCountedThreadSafe<SharedStorageBytes> {
 public:
  inline static ftl::RefPtr<SharedStorageBytes> Create(std::string bytes) {
    return AdoptRef(new SharedStorageBytes(std::move(bytes)));
  }

  const std::string& bytes() { return bytes_; }

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(SharedStorageBytes);
  explicit SharedStorageBytes(std::string bytes) : bytes_(std::move(bytes)) {}
  ~SharedStorageBytes() {}

  std::string bytes_;
};

namespace {
std::string SerializeCommit(
    uint64_t generation,
    int64_t timestamp,
    ObjectIdView root_node_id,
    std::vector<std::unique_ptr<const Commit>> parent_commits) {
  flatbuffers::FlatBufferBuilder builder;

  auto parents_id = builder.CreateVectorOfStructs(
      parent_commits.size(),
      static_cast<std::function<void(size_t, convert::IdStorage*)>>(
          [&parent_commits](size_t i, convert::IdStorage* child_storage) {
            *child_storage = *convert::ToIdStorage(parent_commits[i]->GetId());
          }));

  auto storage = CreateCommitStorage(
      builder, timestamp, generation,
      convert::ToFlatBufferVector(&builder, root_node_id), parents_id);
  builder.Finish(storage);
  return std::string(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                     builder.GetSize());
}
}  // namespace

CommitImpl::CommitImpl(PageStorage* page_storage,
                       CommitId id,
                       int64_t timestamp,
                       uint64_t generation,
                       ObjectIdView root_node_id,
                       std::vector<CommitIdView> parent_ids,
                       ftl::RefPtr<SharedStorageBytes> storage_bytes)
    : page_storage_(page_storage),
      id_(std::move(id)),
      timestamp_(timestamp),
      generation_(generation),
      root_node_id_(root_node_id),
      parent_ids_(std::move(parent_ids)),
      storage_bytes_(std::move(storage_bytes)) {
  FTL_DCHECK(page_storage_ != nullptr);
  FTL_DCHECK(id_ == kFirstPageCommitId ||
             (!parent_ids_.empty() && parent_ids_.size() <= 2));
}

CommitImpl::~CommitImpl() {}

std::unique_ptr<Commit> CommitImpl::FromStorageBytes(
    PageStorage* page_storage,
    CommitId id,
    std::string storage_bytes) {
  FTL_DCHECK(id != kFirstPageCommitId);
  ftl::RefPtr<SharedStorageBytes> storage_ptr =
      SharedStorageBytes::Create(std::move(storage_bytes));

  FTL_DCHECK(CheckValidSerialization(storage_ptr->bytes()));

  const CommitStorage* commit_storage =
      GetCommitStorage(storage_ptr->bytes().data());

  ObjectIdView root_node_id = commit_storage->root_node_id();
  std::vector<CommitIdView> parent_ids;

  for (size_t i = 0; i < commit_storage->parents()->size(); ++i) {
    parent_ids.push_back(commit_storage->parents()->Get(i));
  }
  return std::unique_ptr<Commit>(
      new CommitImpl(page_storage, std::move(id), commit_storage->timestamp(),
                     commit_storage->generation(), root_node_id, parent_ids,
                     std::move(storage_ptr)));
}

std::unique_ptr<Commit> CommitImpl::FromContentAndParents(
    PageStorage* page_storage,
    ObjectIdView root_node_id,
    std::vector<std::unique_ptr<const Commit>> parent_commits) {
  FTL_DCHECK(parent_commits.size() == 1 || parent_commits.size() == 2);

  uint64_t parent_generation = 0;
  for (const auto& commit : parent_commits) {
    parent_generation = std::max(parent_generation, commit->GetGeneration());
  }
  uint64_t generation = parent_generation + 1;

  // Sort commit ids for uniqueness.
  std::sort(parent_commits.begin(), parent_commits.end(),
            [](const std::unique_ptr<const Commit>& c1,
               const std::unique_ptr<const Commit>& c2) {
              return c1->GetId() < c2->GetId();
            });
  // Compute timestamp.
  int64_t timestamp;
  if (parent_commits.size() == 2) {
    timestamp = std::max(parent_commits[0]->GetTimestamp(),
                         parent_commits[1]->GetTimestamp());
  } else {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    timestamp = static_cast<int64_t>(tv.tv_sec) * 1000000000L +
                static_cast<int64_t>(tv.tv_usec) * 1000L;
  }

  std::string storage_bytes = SerializeCommit(
      generation, timestamp, root_node_id, std::move(parent_commits));

  CommitId id = glue::SHA256Hash(storage_bytes);

  return FromStorageBytes(page_storage, std::move(id),
                          std::move(storage_bytes));
}

void CommitImpl::Empty(
    PageStorage* page_storage,
    std::function<void(Status, std::unique_ptr<const Commit>)> callback) {
  TreeNode::Empty(page_storage, [
    page_storage, callback = std::move(callback)
  ](Status s, ObjectId root_node_id) {
    if (s != Status::OK) {
      callback(s, nullptr);
      return;
    }

    ftl::RefPtr<SharedStorageBytes> storage_ptr =
        SharedStorageBytes::Create(std::move(root_node_id));

    auto ptr = std::unique_ptr<Commit>(new CommitImpl(
        page_storage, kFirstPageCommitId.ToString(), 0, 0, storage_ptr->bytes(),
        std::vector<CommitIdView>(), std::move(storage_ptr)));
    callback(Status::OK, std::move(ptr));
  });
}

bool CommitImpl::CheckValidSerialization(ftl::StringView storage_bytes) {
  flatbuffers::Verifier verifier(
      reinterpret_cast<const unsigned char*>(storage_bytes.data()),
      storage_bytes.size());

  if (!VerifyCommitStorageBuffer(verifier)) {
    return false;
  };

  const CommitStorage* commit_storage = GetCommitStorage(storage_bytes.data());
  auto parents = commit_storage->parents();
  if (!parents || parents->size() < 1 || parents->size() > 2) {
    return false;
  }
  return true;
}

std::unique_ptr<Commit> CommitImpl::Clone() const {
  return std::unique_ptr<CommitImpl>(
      new CommitImpl(page_storage_, id_, timestamp_, generation_, root_node_id_,
                     parent_ids_, storage_bytes_));
}

const CommitId& CommitImpl::GetId() const {
  return id_;
}

std::vector<CommitIdView> CommitImpl::GetParentIds() const {
  return parent_ids_;
}

int64_t CommitImpl::GetTimestamp() const {
  return timestamp_;
}

uint64_t CommitImpl::GetGeneration() const {
  return generation_;
}

ObjectIdView CommitImpl::GetRootId() const {
  return root_node_id_;
}

ftl::StringView CommitImpl::GetStorageBytes() const {
  return storage_bytes_->bytes();
}

}  // namespace storage
