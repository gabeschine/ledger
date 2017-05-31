// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_GCS_CLOUD_STORAGE_H_
#define APPS_LEDGER_SRC_GCS_CLOUD_STORAGE_H_

#include <functional>
#include <string>

#include "apps/ledger/src/gcs/status.h"
#include "lib/ftl/macros.h"
#include "mx/socket.h"
#include "mx/vmo.h"

namespace gcs {

class CloudStorage {
 public:
  CloudStorage(){};
  virtual ~CloudStorage(){};

  virtual void UploadObject(const std::string& key,
                            mx::vmo data,
                            const std::function<void(Status)>& callback) = 0;

  virtual void DownloadObject(
      const std::string& key,
      const std::function<void(Status status, uint64_t size, mx::socket data)>&
          callback) = 0;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(CloudStorage);
};

}  // namespace gcs

#endif  // APPS_LEDGER_SRC_GCS_CLOUD_STORAGE_H_
