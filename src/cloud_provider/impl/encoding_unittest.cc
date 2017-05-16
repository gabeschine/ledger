// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_provider/impl/encoding.h"

#include <memory>

#include "apps/ledger/src/cloud_provider/impl/timestamp_conversions.h"
#include "gtest/gtest.h"
#include "lib/ftl/time/time_delta.h"

namespace cloud_provider {
namespace {

// Allows to create correct std::strings with \0 bytes inside from C-style
// string constants.
std::string operator"" _s(const char* str, size_t size) {
  return std::string(str, size);
}

TEST(EncodingTest, Encode) {
  Commit commit(
      "some_id", "some_content",
      std::map<ObjectId, Data>{{"object_a", "data_a"}, {"object_b", "data_b"}});
  std::vector<Commit> commits;
  commits.push_back(std::move(commit));

  std::string encoded;
  EXPECT_TRUE(EncodeCommits(std::move(commits), &encoded));
  EXPECT_EQ(
      "{\"some_idV\":{\"id\":\"some_idV\","
      "\"content\":\"some_contentV\","
      "\"objects\":{"
      "\"object_aV\":\"data_aV\","
      "\"object_bV\":\"data_bV\"},"
      "\"timestamp\":{\".sv\":\"timestamp\"},"
      "\"batch_position\":0,"
      "\"batch_size\":1"
      "}}",
      encoded);
}

TEST(EncodingTest, Decode) {
  std::string json =
      "{\"abcV\":{\"content\":\"xyzV\","
      "\"id\":\"abcV\","
      "\"objects\":{"
      "\"object_aV\":\"aV\","
      "\"object_bV\":\"bV\"},"
      "\"timestamp\":1472722368296"
      "}}";

  std::vector<Record> records;
  EXPECT_TRUE(DecodeMultipleCommits(json, &records));
  ASSERT_EQ(1u, records.size());
  EXPECT_EQ("abc", records.front().commit.id);
  EXPECT_EQ("xyz", records.front().commit.content);
  EXPECT_EQ(2u, records.front().commit.storage_objects.size());
  EXPECT_EQ("a", records.front().commit.storage_objects.at("object_a"));
  EXPECT_EQ("b", records.front().commit.storage_objects.at("object_b"));
  EXPECT_EQ(ServerTimestampToBytes(1472722368296), records.front().timestamp);
  EXPECT_EQ(0u, records.front().batch_position);
  EXPECT_EQ(1u, records.front().batch_size);
}

TEST(EncodingTest, DecodeMultiple) {
  std::string json =
      "{\"id2V\":"
      "{\"content\":\"xyzV\","
      "\"id\":\"id2V\","
      "\"objects\":{"
      "\"object_aV\":\"aV\","
      "\"object_bV\":\"bV\"},"
      "\"timestamp\":1472722368296"
      "},"
      "\"id1V\":"
      "{\"content\":\"bazingaV\","
      "\"id\":\"id1V\","
      "\"timestamp\":42"
      "}}";

  std::vector<Record> records;
  EXPECT_TRUE(DecodeMultipleCommits(json, &records));
  EXPECT_EQ(2u, records.size());

  // Records should be ordered by timestamp.
  EXPECT_EQ("id1", records[0].commit.id);
  EXPECT_EQ("bazinga", records[0].commit.content);
  EXPECT_EQ(0u, records[0].commit.storage_objects.size());
  EXPECT_EQ(ServerTimestampToBytes(42), records[0].timestamp);

  EXPECT_EQ("id2", records[1].commit.id);
  EXPECT_EQ("xyz", records[1].commit.content);
  EXPECT_EQ(2u, records[1].commit.storage_objects.size());
  EXPECT_EQ("a", records[1].commit.storage_objects.at("object_a"));
  EXPECT_EQ("b", records[1].commit.storage_objects.at("object_b"));
  EXPECT_EQ(ServerTimestampToBytes(1472722368296), records[1].timestamp);
}

// Verifies that encoding and JSON parsing we use work with zero bytes within
// strings.
TEST(EncodingTest, EncodeDecodeZeroByte) {
  Commit commit("id\0\0\0"_s, "\0"_s,
                std::map<ObjectId, Data>{{"object_a", "\0\0\0"_s},
                                         {"object_\0"_s, "\0"_s}});
  Commit original_commit = commit.Clone();
  std::vector<Commit> commits;
  commits.push_back(std::move(commit));

  std::string encoded;
  EXPECT_TRUE(EncodeCommits(std::move(commits), &encoded));
  // Encoded commit is sent to server, which replaces the timestamp
  // placeholder with server-side timestamp. We emulate this for testing.
  std::string pattern = "{\".sv\":\"timestamp\"}";
  encoded.replace(encoded.find(pattern), pattern.size(), "42");

  std::vector<Record> output_records;
  EXPECT_TRUE(DecodeMultipleCommits(encoded, &output_records));
  EXPECT_EQ(original_commit, output_records.front().commit);
  EXPECT_EQ(ServerTimestampToBytes(42), output_records.front().timestamp);
}

TEST(EncodingTest, EncodeDecodeBatch) {
  std::vector<Commit> commits;
  commits.push_back(Commit("id_1", "content_1", std::map<ObjectId, Data>{}));
  commits.push_back(Commit("id_2", "content_2", std::map<ObjectId, Data>{}));

  std::string encoded;
  EXPECT_TRUE(EncodeCommits(commits, &encoded));
  // Encoded commit is sent to server, which replaces the timestamp
  // placeholder with server-side timestamp. We emulate this for testing.
  std::string pattern = "{\".sv\":\"timestamp\"}";
  encoded.replace(encoded.find(pattern), pattern.size(), "42");
  encoded.replace(encoded.find(pattern), pattern.size(), "43");
  std::vector<Record> records;
  EXPECT_TRUE(DecodeMultipleCommits(encoded, &records));
  EXPECT_EQ(commits[0], records[0].commit);
  EXPECT_EQ(ServerTimestampToBytes(42), records[0].timestamp);
  EXPECT_EQ(0u, records[0].batch_position);
  EXPECT_EQ(2u, records[0].batch_size);
  EXPECT_EQ(commits[1], records[1].commit);
  EXPECT_EQ(ServerTimestampToBytes(43), records[1].timestamp);
  EXPECT_EQ(1u, records[1].batch_position);
  EXPECT_EQ(2u, records[1].batch_size);
}

}  // namespace
}  // namespace cloud_provider
