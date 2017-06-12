// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/public/data_source.h"

#include "apps/ledger/src/convert/convert.h"
#include "lib/mtl/socket/socket_drainer.h"
#include "mx/vmar.h"

namespace storage {

namespace {

template <typename S>
class StringLikeDataChunk : public DataSource::DataChunk {
 public:
  StringLikeDataChunk(S value) : value_(std::move(value)) {}

 private:
  ftl::StringView Get() override { return convert::ExtendedStringView(value_); }

  S value_;
};

template <typename S>
class StringLikeDataSource : public DataSource {
 public:
  StringLikeDataSource(S value)
      : value_(std::move(value)), size_(value_.size()) {}

 private:
  uint64_t GetSize() override { return size_; }

  void Get(std::function<void(std::unique_ptr<DataChunk>, Status)> callback)
      override {
#ifndef NDEBUG
    FTL_DCHECK(!called_);
    called_ = true;
#endif
    callback(std::make_unique<StringLikeDataChunk<S>>(std::move(value_)),
             Status::DONE);
  }

  S value_;
  uint64_t size_;
#ifndef NDEBUG
  bool called_ = false;
#endif
};

class VmoDataChunk : public DataSource::DataChunk {
 public:
  VmoDataChunk(mx::vmo vmo, uint64_t vmo_size)
      : vmo_(std::move(vmo)), vmo_size_(vmo_size) {}

  mx_status_t Init() {
    uintptr_t allocate_address;
    mx_status_t status = mx::vmar::root_self().allocate(
        0, ToFullPages(vmo_size_), MX_VM_FLAG_CAN_MAP_READ, &vmar_,
        &allocate_address);
    if (status != MX_OK) {
      return status;
    }

    return vmar_.map(0, vmo_, 0, vmo_size_, MX_VM_FLAG_PERM_READ,
                     &mapped_address_);
  }

 private:
  uint64_t ToFullPages(uint64_t value) {
    return (value + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
  }

  ftl::StringView Get() override {
    return ftl::StringView(reinterpret_cast<char*>(mapped_address_), vmo_size_);
  }

  mx::vmo vmo_;
  uint64_t vmo_size_;
  mx::vmar vmar_;
  uintptr_t mapped_address_;
};

class VmoDataSource : public DataSource {
 public:
  VmoDataSource(mx::vmo value) : vmo_(std::move(value)) {
    FTL_DCHECK(vmo_);
    mx_status_t status = vmo_.get_size(&vmo_size_);
    if (status != MX_OK) {
      vmo_.reset();
    }
  }

 private:
  uint64_t GetSize() override { return vmo_size_; }

  void Get(std::function<void(std::unique_ptr<DataChunk>, Status)> callback)
      override {
#ifndef NDEBUG
    FTL_DCHECK(!called_);
    called_ = true;
#endif
    if (!vmo_) {
      callback(nullptr, Status::ERROR);
      return;
    }
    auto data = std::make_unique<VmoDataChunk>(std::move(vmo_), vmo_size_);
    if (data->Init() != MX_OK) {
      callback(nullptr, Status::ERROR);
      return;
    }
    callback(std::move(data), Status::DONE);
  }

  mx::vmo vmo_;
  uint64_t vmo_size_;
#ifndef NDEBUG
  bool called_ = false;
#endif
};

class SocketDataSource : public DataSource, public mtl::SocketDrainer::Client {
 public:
  SocketDataSource(mx::socket socket, uint64_t expected_size)
      : socket_(std::move(socket)),
        expected_size_(expected_size),
        remaining_bytes_(expected_size) {
    FTL_DCHECK(socket_);
  }

 private:
  uint64_t GetSize() override { return expected_size_; }

  void Get(std::function<void(std::unique_ptr<DataChunk>, Status)> callback)
      override {
    FTL_DCHECK(socket_);
    callback_ = std::move(callback);
    socket_drainer_ = std::make_unique<mtl::SocketDrainer>(this);
    socket_drainer_->Start(std::move(socket_));
    socket_.reset();
  }

  void OnDataAvailable(const void* data, size_t num_bytes) override {
    if (num_bytes > remaining_bytes_) {
      FTL_LOG(ERROR) << "Received incorrect number of bytes. Expected: "
                     << expected_size_ << ", but received at least "
                     << (num_bytes - remaining_bytes_) << " more.";
      socket_drainer_.reset();
      callback_(nullptr, Status::ERROR);
      return;
    }

    remaining_bytes_ -= num_bytes;
    callback_(std::make_unique<StringLikeDataChunk<std::string>>(
                  std::string(reinterpret_cast<const char*>(data), num_bytes)),
              Status::TO_BE_CONTINUED);
  }

  void OnDataComplete() override {
    socket_drainer_.reset();
    if (remaining_bytes_ != 0) {
      FTL_LOG(ERROR) << "Received incorrect number of bytes. Expected: "
                     << expected_size_ << ", but received "
                     << (expected_size_ - remaining_bytes_);
      callback_(nullptr, Status::ERROR);
      return;
    }

    callback_(std::make_unique<StringLikeDataChunk<std::string>>(std::string()),
              Status::DONE);
  }

  mx::socket socket_;
  uint64_t expected_size_;
  uint64_t remaining_bytes_;
  std::unique_ptr<mtl::SocketDrainer> socket_drainer_;
  std::function<void(std::unique_ptr<DataChunk>, Status)> callback_;
};

class FlatBufferDataChunk : public DataSource::DataChunk {
 public:
  FlatBufferDataChunk(std::unique_ptr<flatbuffers::FlatBufferBuilder> value)
      : value_(std::move(value)) {}

 private:
  ftl::StringView Get() override {
    return ftl::StringView(reinterpret_cast<char*>(value_->GetBufferPointer()),
                           value_->GetSize());
  }

  std::unique_ptr<flatbuffers::FlatBufferBuilder> value_;
};

}  // namespace

std::unique_ptr<DataSource::DataChunk> DataSource::DataChunk::Create(
    std::string value) {
  return std::make_unique<StringLikeDataChunk<std::string>>(std::move(value));
}

std::unique_ptr<DataSource::DataChunk> DataSource::DataChunk::Create(
    std::unique_ptr<flatbuffers::FlatBufferBuilder> value) {
  return std::make_unique<FlatBufferDataChunk>(std::move(value));
}

std::unique_ptr<DataSource> DataSource::Create(std::string value) {
  return std::make_unique<StringLikeDataSource<std::string>>(std::move(value));
}

std::unique_ptr<DataSource> DataSource::Create(fidl::Array<uint8_t> value) {
  return std::make_unique<StringLikeDataSource<fidl::Array<uint8_t>>>(
      std::move(value));
}

std::unique_ptr<DataSource> DataSource::Create(mx::vmo vmo) {
  return std::make_unique<VmoDataSource>(std::move(vmo));
}

std::unique_ptr<DataSource> DataSource::Create(mx::socket socket,
                                               uint64_t size) {
  return std::make_unique<SocketDataSource>(std::move(socket), size);
}

}  // namespace storage
