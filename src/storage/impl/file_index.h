// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_IMPL_FILE_INDEX_H_
#define APPS_LEDGER_SRC_STORAGE_IMPL_FILE_INDEX_H_

#include "apps/ledger/src/storage/impl/file_index_generated.h"
#include "apps/ledger/src/storage/public/data_source.h"
#include "apps/ledger/src/storage/public/types.h"
#include "lib/ftl/strings/string_view.h"

namespace storage {

// Wrappers over flatbuffer serialization of FileIndex that ensures additional
// validation.
class FileIndexSerialization {
 public:
  struct ObjectIdAndSize {
    ObjectId id;
    uint64_t size;
  };

  // Checks that |data| is a correct encoding for a |FileIndex|.
  static bool CheckValidFileIndexSerialization(ftl::StringView data);

  // Parses a |FileIndex| from |content|.
  static Status ParseFileIndex(ftl::StringView content,
                               const FileIndex** file_index);

  // Builds the |FileIndex| representing the given children.
  static void BuildFileIndex(const std::vector<ObjectIdAndSize>& children,
                             std::unique_ptr<DataSource::DataChunk>* output,
                             size_t* total_size);

 private:
  FileIndexSerialization() {}
};

}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_IMPL_FILE_INDEX_H_
