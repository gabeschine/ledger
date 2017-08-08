// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/test/cloud_server/firebase_server.h"

#include <algorithm>
#include <deque>
#include <map>
#include <sstream>
#include <unordered_map>

#include <magenta/syscalls.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "apps/ledger/src/callback/auto_cleanable.h"
#include "apps/ledger/src/convert/convert.h"
#include "apps/ledger/src/glue/socket/socket_pair.h"
#include "apps/ledger/src/glue/socket/socket_writer.h"
#include "lib/ftl/arraysize.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/concatenate.h"
#include "lib/ftl/strings/split_string.h"
#include "lib/ftl/strings/string_number_conversions.h"
#include "lib/mtl/vmo/strings.h"
#include "lib/url/gurl.h"

namespace ledger {

namespace {
constexpr ftl::StringView kAuth = "auth";
constexpr ftl::StringView kOrderBy = "orderBy";
constexpr ftl::StringView kStartAt = "startAt";

constexpr ftl::StringView kExpectedQueryParameters[] = {kAuth, kOrderBy,
                                                        kStartAt};

// Filter for a Firebase query. |key| is the name of the field to consider, and
// |start_at| is the minimal value of it.
struct Filter {
  std::string key;
  int64_t start_at;
};

// Container for a socket connected to a watcher. This class handles sending a
// stream of data to the socket.
class ListenerContainer : public glue::SocketWriter::Client {
 public:
  explicit ListenerContainer(std::unique_ptr<Filter> filter)
      : writer_(this), filter_(std::move(filter)) {}
  ~ListenerContainer() override {}

  Filter* filter() { return filter_.get(); }

  void Start(mx::socket socket) { writer_.Start(std::move(socket)); }

  void SendChunk(std::string data) {
    FTL_DCHECK(!data.empty());
    content_.push_back(std::move(data));
    CallWriterBack();
  }

  void set_on_empty(ftl::Closure on_done) { on_done_ = std::move(on_done); }

 private:
  void CallWriterBack() {
    if (content_.empty() || !writer_callback_) {
      return;
    }

    FTL_DCHECK(max_size_ > 0);
    ftl::StringView to_send = content_[0];
    to_send = to_send.substr(0, max_size_);
    FTL_DCHECK(!to_send.empty());
    // writer_callback_ needs to be reset before calling it, because GetNext
    // might be called synchronously.
    auto callback = std::move(writer_callback_);
    writer_callback_ = nullptr;
    callback(to_send);
  }

  // glue::SocketWriter::Client
  void GetNext(size_t offset,
               size_t max_size,
               std::function<void(ftl::StringView)> callback) override {
    size_t to_remove = offset - current_offset_;
    while (to_remove > 0) {
      FTL_DCHECK(!content_.empty());
      if (content_[0].size() <= to_remove) {
        to_remove -= content_[0].size();
        content_.pop_front();
      } else {
        content_[0] = content_[0].substr(to_remove);
        to_remove = 0;
      }
    }
    writer_callback_ = std::move(callback);
    current_offset_ = offset;
    max_size_ = max_size;
    CallWriterBack();
  }
  void OnDataComplete() override {
    FTL_DCHECK(on_done_);
    on_done_();
  }

  glue::SocketWriter writer_;
  const std::unique_ptr<Filter> filter_;
  std::deque<std::string> content_;
  ftl::Closure on_done_;
  std::function<void(ftl::StringView)> writer_callback_;
  size_t current_offset_ = 0u;
  size_t max_size_ = 0u;
};

std::string UrlDecode(ftl::StringView value) {
  std::ostringstream result;
  while (!value.empty()) {
    if (value[0] != '%') {
      result << value[0];
      value = value.substr(1);
      continue;
    }
    FTL_DCHECK(value.size() >= 3);
    unsigned char c;
    if (!ftl::StringToNumberWithError(value.substr(1, 2), &c, ftl::Base::k16)) {
      FTL_NOTREACHED();
    }
    result << c;
    value = value.substr(3);
  }
  return result.str();
}

// Serializes the given |value| to a json string. If |filter| is not null,
// values are filtered according to it. Return "null" if |value| is null.
std::string Serialize(rapidjson::Value* value, Filter* filter) {
  if (!value) {
    return "null";
  }
  if (!filter || !value->IsObject()) {
    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value->Accept(writer);
    return buffer.GetString();
  }

  rapidjson::StringBuffer string_buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(string_buffer);
  writer.StartObject();
  for (auto it = value->MemberBegin(); it != value->MemberEnd(); ++it) {
    if (!it->value.IsObject() || !it->value.HasMember(filter->key) ||
        !it->value[filter->key].IsInt64()) {
      FTL_NOTREACHED()
          << "Data does not conform to the expected schema, cannot find field "
          << filter->key << " in " << Serialize(&it->value, nullptr);
    }
    if (it->value[filter->key].GetInt64() >= filter->start_at) {
      writer.Key(it->name.GetString());
      it->value.Accept(writer);
    }
  }
  writer.EndObject();
  return string_buffer.GetString();
}

std::string BuildPathRepresentation(FirebaseServer::PathView path) {
  if (path.empty()) {
    return "/";
  }
  std::ostringstream result;
  for (const auto& element : path) {
    result << "/";
    result << element;
  }
  return result.str();
}

std::string BuildEvent(ftl::StringView event_name,
                       FirebaseServer::PathView path,
                       rapidjson::Value* value,
                       Filter* filter) {
  return ftl::Concatenate({"event: ", event_name, "\ndata: {\"path\":\"",
                           BuildPathRepresentation(path),
                           "\",\"data\":", Serialize(value, filter), "}\n\n"});
}

// Parses |url| and extract the filtering data. Returns an empty unique_ptr if
// not present.
std::unique_ptr<Filter> ExtractFilter(const url::GURL& url) {
  if (!url.has_query()) {
    return nullptr;
  }

  std::string query_string = url.query();
  std::unordered_map<std::string, std::string> queries;
  for (const auto& query : ftl::SplitString(
           query_string, "&", ftl::WhiteSpaceHandling::kKeepWhitespace,
           ftl::SplitResult::kSplitWantAll)) {
    auto split =
        ftl::SplitString(query, "=", ftl::WhiteSpaceHandling::kKeepWhitespace,
                         ftl::SplitResult::kSplitWantAll);
    FTL_DCHECK(split.size() == 2) << "Unparseable query: " << query;
    FTL_DCHECK(std::find(kExpectedQueryParameters,
                         kExpectedQueryParameters +
                             arraysize(kExpectedQueryParameters),
                         split[0]) !=
               kExpectedQueryParameters + arraysize(kExpectedQueryParameters))
        << "Unknown query parameter: " << split[0];
    queries[UrlDecode(split[0])] = UrlDecode(split[1]);
  }

  FTL_DCHECK(queries.count(kOrderBy.ToString()) ==
             queries.count(kStartAt.ToString()))
      << "Both orderBy and startAt must be present.";
  if (!queries.count(kOrderBy.ToString())) {
    return nullptr;
  }

  std::string& order_by = queries[kOrderBy.ToString()];
  FTL_DCHECK(order_by[0] == '"' && order_by[order_by.size() - 1] == '"')
      << order_by;
  FTL_DCHECK(std::find(order_by.begin(), order_by.end(), '/') == order_by.end())
      << "Not handling complex path in orderBy";
  order_by = order_by.substr(1, order_by.size() - 2);
  std::string& start_at = queries[kStartAt.ToString()];

  auto result = std::make_unique<Filter>();
  result->key = order_by;
  if (!ftl::StringToNumberWithError(start_at, &result->start_at)) {
    FTL_NOTREACHED() << "Invalid filter, " << start_at << " is not an int.";
  }
  return result;
}

bool IsTimestamp(const rapidjson::Value& value) {
  if (!value.IsObject()) {
    return false;
  }

  if (value.MemberCount() != 1) {
    return false;
  }

  if (!value.HasMember(".sv")) {
    return false;
  }
  const auto& sv = value[".sv"];
  if (!sv.IsString()) {
    return false;
  }
  return sv == "timestamp";
}

// Recurse through |value| and replace the following json object:
// { ".sv": "timestamp" } by the given |timestamp| or the current time if
// timestamp == MIN_INT64
void FillTimestamp(rapidjson::Value* value,
                   rapidjson::Document* document,
                   int64_t timestamp = std::numeric_limits<int64_t>::min()) {
  if (!value->IsObject()) {
    return;
  }

  if (timestamp == std::numeric_limits<int64_t>::min()) {
    timestamp = mx_time_get(MX_CLOCK_UTC) / 1000;
  }

  std::vector<std::string> elements_to_change;
  for (auto it = value->MemberBegin(); it != value->MemberEnd(); ++it) {
    if (IsTimestamp(it->value)) {
      elements_to_change.emplace_back(it->name.GetString());
    } else {
      FillTimestamp(&it->value, document, timestamp);
    }
  }
  rapidjson::Value ts(timestamp);
  for (const auto& name : elements_to_change) {
    rapidjson::Value key(name.c_str(), document->GetAllocator());
    value->RemoveMember(key);
    value->AddMember(key, ts, document->GetAllocator());
  }
}

FirebaseServer::Path GetPath(const url::GURL& url) {
  constexpr ftl::StringView json_suffix = ".json";

  std::string path = url.path();
  ftl::StringView path_view = path;
  FTL_DCHECK(path_view[0] == '/');
  path_view = path_view.substr(1);
  FTL_DCHECK(path_view.substr(path_view.size() - json_suffix.size()) ==
             json_suffix);
  path_view = path_view.substr(0, path_view.size() - json_suffix.size());
  return ftl::SplitStringCopy(path_view, "/",
                              ftl::WhiteSpaceHandling::kKeepWhitespace,
                              ftl::SplitResult::kSplitWantAll);
}
}  // namespace

class FirebaseServer::Listeners {
 public:
  Listeners() {}
  ~Listeners() {}

  void AddListener(PathView path,
                   std::unique_ptr<Filter> filter,
                   mx::socket socket,
                   rapidjson::Value* initial_value);

  void SendEvent(const std::string& event_name,
                 PathView path,
                 rapidjson::Value* value);

 private:
  std::unordered_map<std::string, Listeners> children_;
  callback::AutoCleanableSet<ListenerContainer> listeners_;
};

void FirebaseServer::Listeners::AddListener(PathView path,
                                            std::unique_ptr<Filter> filter,
                                            mx::socket socket,
                                            rapidjson::Value* initial_value) {
  if (!path.empty()) {
    children_[path[0]].AddListener(path.Tail(), std::move(filter),
                                   std::move(socket), initial_value);
    return;
  }
  auto& new_listener = listeners_.emplace(std::move(filter));
  new_listener.Start(std::move(socket));
  Path empty_path;
  new_listener.SendChunk(
      BuildEvent("put", empty_path, initial_value, new_listener.filter()));
}

void FirebaseServer::Listeners::SendEvent(const std::string& event_name,
                                          PathView path,
                                          rapidjson::Value* value) {
  for (auto& listener : listeners_) {
    listener.SendChunk(BuildEvent(event_name, path, value, listener.filter()));
  }

  if (!path.empty()) {
    if (children_.count(path[0]) != 0) {
      children_[path[0]].SendEvent(event_name, path.Tail(), value);
    }
    return;
  }
  if (value == nullptr || !value->IsObject()) {
    return;
  }
  for (auto it = value->MemberBegin(); it != value->MemberEnd(); ++it) {
    auto key = it->name.GetString();
    if (children_.count(key) != 0) {
      children_[key].SendEvent(event_name, path, &it->value);
    }
  }
}

FirebaseServer::FirebaseServer() : listeners_(std::make_unique<Listeners>()) {
  document_.SetObject();
}

FirebaseServer::~FirebaseServer() {}

void FirebaseServer::HandleGet(
    network::URLRequestPtr request,
    const std::function<void(network::URLResponsePtr)> callback) {
  url::GURL url(request->url);
  callback(BuildResponse(request->url, Server::ResponseCode::kOk,
                         GetSerializedValueForURL(url)));
}

void FirebaseServer::HandleGetStream(
    network::URLRequestPtr request,
    const std::function<void(network::URLResponsePtr)> callback) {
  url::GURL url(request->url);
  auto path = GetPath(url);
  glue::SocketPair sockets;
  listeners_->AddListener(path, ExtractFilter(url), std::move(sockets.socket1),
                          GetValueAtPath(path));
  callback(BuildResponse(request->url, Server::ResponseCode::kOk,
                         std::move(sockets.socket2), {}));
}

void FirebaseServer::HandlePatch(
    network::URLRequestPtr request,
    const std::function<void(network::URLResponsePtr)> callback) {
  std::string body;
  if (!mtl::StringFromVmo(request->body->get_buffer(), &body)) {
    FTL_NOTREACHED();
  }

  url::GURL url(request->url);
  auto path = GetPath(url);
  rapidjson::Value* value = GetValueAtPath(path, true);

  rapidjson::Document new_value;
  new_value.Parse(body.c_str(), body.size());
  FTL_DCHECK(!new_value.HasParseError());

  FillTimestamp(&new_value, &new_value);

  for (auto it = new_value.MemberBegin(); it != new_value.MemberEnd(); ++it) {
    if (value->HasMember(it->name.GetString())) {
      // Ledger database is configured to prevent data overwritting.
      callback(BuildResponse(request->url, Server::ResponseCode::kUnauthorized,
                             "Data already exists"));
      return;
    }
  }

  for (auto it = new_value.MemberBegin(); it != new_value.MemberEnd(); ++it) {
    rapidjson::Value key(it->name.GetString(), document_.GetAllocator());
    rapidjson::Value copied_value(it->value, document_.GetAllocator());
    value->AddMember(key, copied_value, document_.GetAllocator());
  }

  callback(BuildResponse(request->url, Server::ResponseCode::kOk,
                         Serialize(&new_value, nullptr)));

  listeners_->SendEvent("patch", path, &new_value);
}

void FirebaseServer::HandlePut(
    network::URLRequestPtr request,
    const std::function<void(network::URLResponsePtr)> callback) {
  std::string body;
  if (!mtl::StringFromVmo(request->body->get_buffer(), &body)) {
    FTL_NOTREACHED();
  }

  url::GURL url(request->url);
  auto path = GetPath(url);
  FTL_DCHECK(!path.empty());
  auto sub_path = PathView(path, path.begin(), path.end() - 1);
  rapidjson::Value* value = GetValueAtPath(sub_path, true);

  if (value->HasMember(path.back())) {
    // Ledger database is configured to prevent data overwritting.
    callback(BuildResponse(request->url, Server::ResponseCode::kUnauthorized,
                           "Data already exists"));
  }

  rapidjson::Document new_value;
  new_value.Parse(body.c_str(), body.size());
  FTL_DCHECK(!new_value.HasParseError());

  FillTimestamp(&new_value, &new_value);
  rapidjson::Value key(path.back().c_str(), document_.GetAllocator());
  rapidjson::Value copied_value(new_value, document_.GetAllocator());
  value->AddMember(key, copied_value, document_.GetAllocator());

  callback(BuildResponse(request->url, Server::ResponseCode::kOk,
                         Serialize(&new_value, nullptr)));
  listeners_->SendEvent("put", path, &new_value);
}

std::string FirebaseServer::GetSerializedValueForURL(const url::GURL& url) {
  auto path = GetPath(url);
  std::string initial_value;
  rapidjson::Value* value = GetValueAtPath(path);
  if (!value) {
    return "null";
  }
  auto filter = ExtractFilter(url);
  return Serialize(value, filter.get());
}

rapidjson::Value* FirebaseServer::GetValueAtPath(PathView path, bool create) {
  rapidjson::Value* value = &document_;
  for (const auto& element : path) {
    if (!value->IsObject()) {
      return nullptr;
    }
    if (!value->HasMember(element)) {
      if (!create) {
        return nullptr;
      }
      rapidjson::Value key(element.c_str(), document_.GetAllocator());
      rapidjson::Value object(rapidjson::kObjectType);
      value->AddMember(key, object, document_.GetAllocator());
    }

    value = &((*value)[element]);
  }

  return value;
}

}  // namespace ledger
