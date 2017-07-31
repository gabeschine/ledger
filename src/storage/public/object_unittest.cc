// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/public/object.h"

#include "gtest/gtest.h"
#include "lib/mtl/vmo/strings.h"

namespace storage {
namespace {

class StringObject : public storage::Object {
 public:
  explicit StringObject(std::string value) : value_(std::move(value)) {}
  ~StringObject() override {}

  ObjectId GetId() const override { return "id"; }

  virtual Status GetData(ftl::StringView* data) const override {
    *data = value_;
    return Status::OK;
  }

 private:
  std::string value_;
};

TEST(ObjectTest, GetVmo) {
  std::string content = "content";
  StringObject object(content);

  mx::vmo vmo;
  ASSERT_EQ(Status::OK, object.GetVmo(&vmo));
  std::string vmo_content;
  ASSERT_TRUE(mtl::StringFromVmo(vmo, &vmo_content));
  EXPECT_EQ(content, vmo_content);
}

}  // namespace
}  // namespace storage
