// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_GLUE_CRYPTO_HASH_H_
#define APPS_LEDGER_SRC_GLUE_CRYPTO_HASH_H_

#include <memory>
#include <string>

#include "lib/ftl/macros.h"
#include "lib/ftl/strings/string_view.h"

namespace glue {

class SHA256StreamingHash {
 public:
  SHA256StreamingHash();
  ~SHA256StreamingHash();
  void Update(ftl::StringView data);
  void Finish(std::string* output);

 private:
  struct Context;
  std::unique_ptr<Context> context_;

  FTL_DISALLOW_COPY_AND_ASSIGN(SHA256StreamingHash);
};

std::string SHA256Hash(const void* input, size_t input_lenght);

}  // namespace glue

#endif  // APPS_LEDGER_SRC_GLUE_CRYPTO_HASH_H_
