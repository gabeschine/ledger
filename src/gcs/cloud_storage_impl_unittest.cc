// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/gcs/cloud_storage_impl.h"

#include <memory>
#include <string>
#include <utility>

#include "apps/ledger/src/callback/capture.h"
#include "apps/ledger/src/network/fake_network_service.h"
#include "apps/ledger/src/test/test_with_message_loop.h"
#include "apps/network/services/network_service.fidl.h"
#include "gtest/gtest.h"
#include "lib/ftl/files/file.h"
#include "lib/ftl/files/scoped_temp_dir.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/strings/string_number_conversions.h"
#include "lib/mtl/socket/strings.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/vmo/strings.h"

namespace gcs {
namespace {

network::HttpHeaderPtr GetHeader(
    const fidl::Array<network::HttpHeaderPtr>& headers,
    const std::string& header_name) {
  for (const auto& header : headers.storage()) {
    if (header->name == header_name) {
      return header.Clone();
    }
  }
  return nullptr;
}

class CloudStorageImplTest : public test::TestWithMessageLoop {
 public:
  CloudStorageImplTest()
      : fake_network_service_(message_loop_.task_runner()),
        gcs_(message_loop_.task_runner(),
             &fake_network_service_,
             "project",
             "prefix") {}
  ~CloudStorageImplTest() override {}

 protected:
  void SetResponse(const std::string& body,
                   int64_t content_length,
                   uint32_t status_code) {
    network::URLResponsePtr server_response = network::URLResponse::New();
    server_response->body = network::URLBody::New();
    server_response->body->set_stream(mtl::WriteStringToSocket(body));
    server_response->status_code = status_code;

    network::HttpHeaderPtr content_length_header = network::HttpHeader::New();
    content_length_header->name = "content-length";
    content_length_header->value = ftl::NumberToString(content_length);

    server_response->headers.push_back(std::move(content_length_header));

    fake_network_service_.SetResponse(std::move(server_response));
  }

  bool CreateFile(const std::string& content, std::string* path) {
    if (!tmp_dir_.NewTempFile(path))
      return false;
    return files::WriteFile(*path, content.data(), content.size());
  }

  files::ScopedTempDir tmp_dir_;
  ledger::FakeNetworkService fake_network_service_;
  CloudStorageImpl gcs_;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(CloudStorageImplTest);
};

TEST_F(CloudStorageImplTest, TestUpload) {
  std::string content = "Hello World\n";
  mx::vmo data;
  ASSERT_TRUE(mtl::VmoFromString(content, &data));

  SetResponse("", 0, 200);
  Status status;
  gcs_.UploadObject(
      "hello-world", std::move(data),
      callback::Capture([this] { message_loop_.PostQuitTask(); }, &status));
  ASSERT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(
      "https://firebasestorage.googleapis.com"
      "/v0/b/project.appspot.com/o/prefixhello-world",
      fake_network_service_.GetRequest()->url);
  EXPECT_EQ("POST", fake_network_service_.GetRequest()->method);
  EXPECT_TRUE(fake_network_service_.GetRequest()->body->is_buffer());
  std::string sent_content;
  EXPECT_TRUE(mtl::StringFromVmo(
      std::move(fake_network_service_.GetRequest()->body->get_buffer()),
      &sent_content));
  EXPECT_EQ(content, sent_content);

  network::HttpHeaderPtr content_length_header =
      GetHeader(fake_network_service_.GetRequest()->headers, "content-length");
  EXPECT_TRUE(content_length_header);
  unsigned content_length;
  EXPECT_TRUE(ftl::StringToNumberWithError(content_length_header->value.get(),
                                           &content_length));
  EXPECT_EQ(content.size(), content_length);

  network::HttpHeaderPtr if_generation_match_header =
      GetHeader(fake_network_service_.GetRequest()->headers,
                "x-goog-if-generation-match");
  EXPECT_TRUE(if_generation_match_header);
  EXPECT_EQ("0", if_generation_match_header->value);
}

TEST_F(CloudStorageImplTest, TestUploadWhenObjectAlreadyExists) {
  std::string content = "";
  mx::vmo data;
  ASSERT_TRUE(mtl::VmoFromString(content, &data));
  SetResponse("", 0, 412);

  Status status;
  gcs_.UploadObject(
      "hello-world", std::move(data),
      callback::Capture([this] { message_loop_.PostQuitTask(); }, &status));
  ASSERT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(Status::OBJECT_ALREADY_EXISTS, status);
}

TEST_F(CloudStorageImplTest, TestDownload) {
  const std::string content = "Hello World\n";
  SetResponse(content, content.size(), 200);

  Status status;
  uint64_t size;
  mx::socket data;
  gcs_.DownloadObject(
      "hello-world", callback::Capture([this] { message_loop_.PostQuitTask(); },
                                       &status, &size, &data));
  ASSERT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(
      "https://firebasestorage.googleapis.com"
      "/v0/b/project.appspot.com/o/prefixhello-world?alt=media",
      fake_network_service_.GetRequest()->url);
  EXPECT_EQ("GET", fake_network_service_.GetRequest()->method);

  std::string downloaded_content;
  EXPECT_TRUE(mtl::BlockingCopyToString(std::move(data), &downloaded_content));
  EXPECT_EQ(downloaded_content, content);
  EXPECT_EQ(size, content.size());
}

TEST_F(CloudStorageImplTest, TestDownloadNotFound) {
  SetResponse("", 0, 404);

  Status status;
  uint64_t size;
  mx::socket data;
  gcs_.DownloadObject(
      "whoa", callback::Capture([this] { message_loop_.PostQuitTask(); },
                                &status, &size, &data));
  ASSERT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(Status::NOT_FOUND, status);
  EXPECT_EQ(
      "https://firebasestorage.googleapis.com"
      "/v0/b/project.appspot.com/o/prefixwhoa?alt=media",
      fake_network_service_.GetRequest()->url);
  EXPECT_EQ("GET", fake_network_service_.GetRequest()->method);
}

TEST_F(CloudStorageImplTest, TestDownloadWithResponseBodyTooShort) {
  const std::string content = "abc";
  SetResponse(content, content.size() + 1, 200);

  Status status;
  uint64_t size;
  mx::socket data;
  gcs_.DownloadObject(
      "hello-world", callback::Capture([this] { message_loop_.PostQuitTask(); },
                                       &status, &size, &data));
  ASSERT_FALSE(RunLoopWithTimeout());

  std::string downloaded_content;
  EXPECT_TRUE(mtl::BlockingCopyToString(std::move(data), &downloaded_content));

  // As the result is returned in a socket, we pass the expected size to the
  // client so that they can verify if the response is complete.
  EXPECT_EQ(Status::OK, status);
  EXPECT_EQ(4u, size);
  EXPECT_EQ(3u, downloaded_content.size());
}

}  // namespace
}  // namespace gcs
