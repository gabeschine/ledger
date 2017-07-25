// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/split.h"

#include <limits>
#include <sstream>

#include "apps/ledger/src/callback/waiter.h"
#include "apps/ledger/src/glue/crypto/hash.h"
#include "apps/ledger/src/storage/impl/constants.h"
#include "apps/ledger/src/storage/impl/file_index.h"
#include "apps/ledger/src/storage/impl/file_index_generated.h"
#include "apps/ledger/src/storage/impl/object_id.h"
#include "apps/ledger/src/storage/public/data_source.h"
#include "apps/ledger/src/third_party/bup/bupsplit.h"
#include "lib/ftl/functional/closure.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/macros.h"

namespace storage {

namespace {
constexpr size_t kMinChunkSize = 4 * 1024;
constexpr size_t kMaxChunkSize = std::numeric_limits<uint16_t>::max();
constexpr size_t kBitsPerLevel = 4;
// The max number of indentifiers that an index can contain so that the file
// size is less than |kMaxChunkSize|.
constexpr size_t kMaxIdentifiersPerIndex = kMaxChunkSize / 61;

using ObjectIdAndSize = FileIndexSerialization::ObjectIdAndSize;

struct ChunkAndSize {
  std::unique_ptr<DataSource::DataChunk> chunk;
  uint64_t size;
};

// Handles the successive callbacks from the DataSource.
//
// Algorithm:
// This class keeps track of a list of identifiers per level. For each level,
// the list must be aggregated into an index file, or if alone at the highest
// level when the algorithm ends, sent to the client.
// The algorithm reads data from the source and feeds it to the rolling hash.
// For each chunk cut by the rolling hash, the identifier of the chunk is added
// at level 0. The rolling hash algorithm also returns the number of index files
// that need to be built. An index file is also built as soon as a level
// contains |kMaxIdentifiersPerIndex| identifiers.
// When the algorithm builds the index at level |n| it does the following:
// For all levels from 0 to |n|:
//   - Build the index file at the given level. As a special case, if there is
//     a single object at the given level, just move it to the next level and
//     continue.
//   - Send the index file to the client.
//   - Add the identifier of the index file at the next level.
class SplitContext {
 public:
  SplitContext(
      std::function<void(IterationStatus,
                         ObjectId,
                         std::unique_ptr<DataSource::DataChunk>)> callback)
      : callback_(std::move(callback)),
        roll_sum_split_(kMinChunkSize, kMaxChunkSize) {}
  SplitContext(SplitContext&& other) = default;
  ~SplitContext() {}

  void AddChunk(std::unique_ptr<DataSource::DataChunk> chunk,
                DataSource::Status status) {
    if (status == DataSource::Status::ERROR) {
      callback_(IterationStatus::ERROR, "", nullptr);
      return;
    }

    FTL_DCHECK(chunk || status == DataSource::Status::DONE);

    if (chunk) {
      ProcessChunk(std::move(chunk));
    }

    if (status != DataSource::Status::DONE) {
      return;
    }

    if (!current_chunks_.empty()) {
      // The remaining data needs to be sent even if it is not chunked at an
      // expected cut point.
      BuildAndSendNextChunk(views_.back().size());
    }

    // No data remains.
    FTL_DCHECK(current_chunks_.empty());

    // The final id to send exists.
    FTL_DCHECK(!current_identifiers_per_level_.back().empty());

    // This traverses the stack of indices, sending each level until a single
    // top level index is produced.
    for (size_t i = 0; i < current_identifiers_per_level_.size(); ++i) {
      if (current_identifiers_per_level_[i].empty()) {
        continue;
      }

      // At the top of the stack with a single element, the algorithm is
      // finished. The top-level object_id is the unique element.
      if (i == current_identifiers_per_level_.size() - 1 &&
          current_identifiers_per_level_[i].size() == 1) {
        callback_(IterationStatus::DONE,
                  std::move(current_identifiers_per_level_[i][0].id), nullptr);
        return;
      }

      BuildIndexAtLevel(i);
    }

    FTL_NOTREACHED();
  }

 private:
  std::vector<ObjectIdAndSize>& GetCurrentIdentifiersAtLevel(size_t level) {
    if (level >= current_identifiers_per_level_.size()) {
      FTL_DCHECK(level == current_identifiers_per_level_.size());
      current_identifiers_per_level_.resize(level + 1);
    }
    return current_identifiers_per_level_[level];
  }

  // Appends the given chunk to the unprocessed data and processes as much data
  // as possible using the rolling hash to determine where to cut the stream in
  // pieces.
  void ProcessChunk(std::unique_ptr<DataSource::DataChunk> chunk) {
    views_.push_back(chunk->Get());
    current_chunks_.push_back(std::move(chunk));

    while (!views_.empty()) {
      size_t bits;
      size_t split_index = roll_sum_split_.Feed(views_.back(), &bits);

      if (split_index == 0) {
        return;
      }

      BuildAndSendNextChunk(split_index);

      size_t level = GetLevel(bits);
      for (size_t i = 0; i < level; ++i) {
        FTL_DCHECK(!current_identifiers_per_level_[i].empty());
        BuildIndexAtLevel(i);
      }
    }
  }

  void BuildAndSendNextChunk(size_t split_index) {
    std::unique_ptr<DataSource::DataChunk> data = BuildNextChunk(split_index);
    auto data_view = data->Get();
    size_t size = data_view.size();
    ObjectId object_id = ComputeObjectId(ObjectType::VALUE, data_view);
    callback_(IterationStatus::IN_PROGRESS, object_id, std::move(data));
    AddIdentifierAtLevel(0, {std::move(object_id), size});
  }

  void AddIdentifierAtLevel(size_t level, ObjectIdAndSize data) {
    GetCurrentIdentifiersAtLevel(level).push_back(std::move(data));

    if (current_identifiers_per_level_[level].size() <
        kMaxIdentifiersPerIndex) {
      // The level is not full, more identifiers can be added.
      return;
    }

    FTL_DCHECK(current_identifiers_per_level_[level].size() ==
               kMaxIdentifiersPerIndex);
    // The level contains the max number of identifiers. Creating the index
    // file.

    AddIdentifierAtLevel(
        level + 1,
        BuildAndSendIndex(std::move(current_identifiers_per_level_[level])));
    current_identifiers_per_level_[level].clear();
  }

  void BuildIndexAtLevel(size_t level) {
    auto objects = std::move(current_identifiers_per_level_[level]);
    current_identifiers_per_level_[level].clear();

    if (objects.size() == 1) {
      AddIdentifierAtLevel(level + 1, std::move(objects.front()));
    } else {
      auto id_and_size = BuildAndSendIndex(std::move(objects));
      AddIdentifierAtLevel(level + 1, std::move(id_and_size));
    }
  }

  ObjectIdAndSize BuildAndSendIndex(std::vector<ObjectIdAndSize> ids) {
    FTL_DCHECK(ids.size() > 1);
    FTL_DCHECK(ids.size() <= kMaxIdentifiersPerIndex);

    std::unique_ptr<DataSource::DataChunk> chunk;
    size_t total_size;
    FileIndexSerialization::BuildFileIndex(ids, &chunk, &total_size);

    FTL_DCHECK(chunk->Get().size() <= kMaxChunkSize) << chunk->Get().size();
    ObjectId object_id = ComputeObjectId(ObjectType::INDEX, chunk->Get());
    callback_(IterationStatus::IN_PROGRESS, object_id, std::move(chunk));
    return {std::move(object_id), total_size};
  }

  static size_t GetLevel(size_t bits) {
    FTL_DCHECK(bits >= bup::kBlobBits);
    return (bits - bup::kBlobBits) / kBitsPerLevel;
  }

  std::unique_ptr<DataSource::DataChunk> BuildNextChunk(size_t index) {
    FTL_DCHECK(current_chunks_.size() == views_.size());
    FTL_DCHECK(!current_chunks_.empty());
    FTL_DCHECK(views_.back().size() >= index);

    if (views_.size() == 1 && views_.front().size() == index &&
        views_.front().size() == current_chunks_.front()->Get().size()) {
      std::unique_ptr<DataSource::DataChunk> result =
          std::move(current_chunks_.front());
      views_.clear();
      current_chunks_.clear();
      return result;
    }

    std::string data;
    size_t total_size = index;

    for (size_t i = 0; i + 1 < views_.size(); ++i) {
      total_size += views_[i].size();
    }
    data.reserve(total_size);
    for (size_t i = 0; i + 1 < views_.size(); ++i) {
      data.append(views_[i].data(), views_[i].size());
    }

    ftl::StringView last = views_.back();
    data.append(last.data(), index);

    if (index < last.size()) {
      views_.clear();
      if (current_chunks_.size() > 1) {
        std::swap(current_chunks_.front(), current_chunks_.back());
        current_chunks_.resize(1);
      }
      views_.push_back(last.substr(index));
    } else {
      current_chunks_.clear();
      views_.clear();
    }

    FTL_DCHECK(current_chunks_.size() == views_.size());
    return DataSource::DataChunk::Create(std::move(data));
  }

  std::function<
      void(IterationStatus, ObjectId, std::unique_ptr<DataSource::DataChunk>)>
      callback_;
  bup::RollSumSplit roll_sum_split_;
  // The list of chunks from the initial source that are not yet entiretly
  // consumed.
  std::vector<std::unique_ptr<DataSource::DataChunk>> current_chunks_;
  // The list of data that has not yet been consumed. For all indexes, the view
  // at the given index is a view to the chunk at the same index.
  std::vector<ftl::StringView> views_;
  // List of unsent indices per level.
  std::vector<std::vector<ObjectIdAndSize>> current_identifiers_per_level_;

  FTL_DISALLOW_COPY_AND_ASSIGN(SplitContext);
};

class CollectPiecesState
    : public ftl::RefCountedThreadSafe<CollectPiecesState> {
 public:
  std::function<void(ObjectIdView,
                     std::function<void(Status, ftl::StringView)>)>
      data_accessor;
  std::function<bool(IterationStatus, ObjectIdView)> callback;
  bool running = true;
};

void CollectPiecesInternal(ObjectIdView root,
                           ftl::RefPtr<CollectPiecesState> state,
                           ftl::Closure on_done) {
  if (!state->callback(IterationStatus::IN_PROGRESS, root)) {
    on_done();
    return;
  }

  if (GetObjectIdType(root) != ObjectIdType::INDEX_HASH) {
    on_done();
    return;
  }

  state->data_accessor(root, [ state, on_done = std::move(on_done) ](
                                 Status status, ftl::StringView data) mutable {
    if (!state->running) {
      on_done();
      return;
    }

    if (status != Status::OK) {
      FTL_LOG(WARNING) << "Unable to read object content.";
      state->running = false;
      on_done();
      return;
    }

    auto waiter = callback::CompletionWaiter::Create();
    status = ForEachPiece(data, [&](ObjectIdView id) {
      CollectPiecesInternal(id, state, waiter->NewCallback());
      return Status::OK;
    });
    if (status != Status::OK) {
      state->running = false;
      on_done();
      return;
    }

    waiter->Finalize(std::move(on_done));
  });
}

}  // namespace

void SplitDataSource(
    DataSource* source,
    std::function<void(IterationStatus,
                       ObjectId,
                       std::unique_ptr<DataSource::DataChunk>)> callback) {
  SplitContext context(std::move(callback));
  source->Get(ftl::MakeCopyable([context = std::move(context)](
      std::unique_ptr<DataSource::DataChunk> chunk,
      DataSource::Status status) mutable {
    context.AddChunk(std::move(chunk), status);
  }));
}

Status ForEachPiece(ftl::StringView index_content,
                    std::function<Status(ObjectIdView)> callback) {
  const FileIndex* file_index;
  Status status =
      FileIndexSerialization::ParseFileIndex(index_content, &file_index);
  if (status != Status::OK) {
    return status;
  }

  for (const auto* child : *file_index->children()) {
    Status status = callback(child->object_id());
    if (status != Status::OK) {
      return status;
    }
  }

  return Status::OK;
}

void CollectPieces(
    ObjectIdView root,
    std::function<void(ObjectIdView,
                       std::function<void(Status, ftl::StringView)>)>
        data_accessor,
    std::function<bool(IterationStatus, ObjectIdView)> callback) {
  auto state = ftl::AdoptRef(new CollectPiecesState());
  state->data_accessor = std::move(data_accessor);
  state->callback = std::move(callback);

  CollectPiecesInternal(root, state, [state] {
    IterationStatus final_status =
        state->running ? IterationStatus::DONE : IterationStatus::ERROR;
    state->callback(final_status, "");
  });
}

}  // namespace storage
