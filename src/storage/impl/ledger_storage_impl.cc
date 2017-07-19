// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/ledger_storage_impl.h"

#include <algorithm>
#include <iterator>

#include <dirent.h>

#include "apps/ledger/src/glue/crypto/base64.h"
#include "apps/ledger/src/storage/impl/directory_reader.h"
#include "apps/ledger/src/storage/impl/page_storage_impl.h"
#include "apps/ledger/src/storage/public/constants.h"
#include "lib/ftl/files/directory.h"
#include "lib/ftl/files/path.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/concatenate.h"

namespace storage {

namespace {

// Encodes opaque bytes in a way that is usable as a directory name.
std::string GetDirectoryName(ftl::StringView bytes) {
  std::string encoded;
  glue::Base64UrlEncode(bytes, &encoded);
  return encoded;
}

// Decodes opaque bytes used as a directory names into an id. This is the
// opposite transformation of GetDirectoryName.
std::string GetObjectId(ftl::StringView bytes) {
  std::string decoded;
  glue::Base64UrlDecode(bytes, &decoded);
  return decoded;
}

}  // namespace

LedgerStorageImpl::LedgerStorageImpl(
    coroutine::CoroutineService* coroutine_service,
    const std::string& base_storage_dir,
    const std::string& ledger_name)
    : coroutine_service_(coroutine_service) {
  storage_dir_ = ftl::Concatenate({base_storage_dir, "/", kSerializationVersion,
                                   "/", GetDirectoryName(ledger_name)});
}

LedgerStorageImpl::~LedgerStorageImpl() {}

void LedgerStorageImpl::CreatePageStorage(
    PageId page_id,
    std::function<void(Status, std::unique_ptr<PageStorage>)> callback) {
  std::string path = GetPathFor(page_id);
  if (!files::CreateDirectory(path)) {
    FTL_LOG(ERROR) << "Failed to create the storage directory in " << path;
    callback(Status::INTERNAL_IO_ERROR, nullptr);
    return;
  }
  auto result = std::make_unique<PageStorageImpl>(coroutine_service_, path,
                                                  std::move(page_id));
  result->Init(ftl::MakeCopyable([
    callback = std::move(callback), result = std::move(result)
  ](Status status) mutable {
    if (status != Status::OK) {
      FTL_LOG(ERROR) << "Failed to initialize PageStorage. Status: " << status;
      callback(status, nullptr);
      return;
    }
    callback(Status::OK, std::move(result));
  }));
}

void LedgerStorageImpl::GetPageStorage(
    PageId page_id,
    const std::function<void(Status, std::unique_ptr<PageStorage>)>& callback) {
  std::string path = GetPathFor(page_id);
  if (files::IsDirectory(path)) {
    auto result = std::make_unique<PageStorageImpl>(coroutine_service_, path,
                                                    std::move(page_id));
    result->Init(ftl::MakeCopyable([
      callback = std::move(callback), result = std::move(result)
    ](Status status) mutable {
      if (status != Status::OK) {
        callback(status, nullptr);
        return;
      }
      callback(status, std::move(result));
    }));
    return;
  }
  // TODO(nellyv): Maybe the page exists but is not synchronized, yet. We need
  // to check in the cloud.
  callback(Status::NOT_FOUND, nullptr);
}

bool LedgerStorageImpl::DeletePageStorage(PageIdView page_id) {
  // TODO(nellyv): We need to synchronize the page deletion with the cloud.
  std::string path = GetPathFor(page_id);
  if (!files::IsDirectory(path)) {
    return false;
  }
  if (!files::DeletePath(path, true)) {
    FTL_LOG(ERROR) << "Unable to delete: " << path;
    return false;
  }
  return true;
}

std::vector<PageId> LedgerStorageImpl::ListLocalPages() {
  std::vector<PageId> local_pages;
  DirectoryReader::GetDirectoryEntries(
      storage_dir_, [&local_pages](ftl::StringView encoded_page_id) {
        local_pages.emplace_back(GetObjectId(encoded_page_id));
        return true;
      });
  return local_pages;
}

std::string LedgerStorageImpl::GetPathFor(PageIdView page_id) {
  FTL_DCHECK(!page_id.empty());
  return ftl::Concatenate({storage_dir_, "/", GetDirectoryName(page_id)});
}

}  // namespace storage
