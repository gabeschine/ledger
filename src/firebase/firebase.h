// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_FIREBASE_FIREBASE_H_
#define APPS_LEDGER_SRC_FIREBASE_FIREBASE_H_

#include <functional>
#include <string>
#include <vector>

#include "apps/ledger/src/firebase/status.h"
#include "apps/ledger/src/firebase/watch_client.h"
#include "lib/ftl/macros.h"

#include <rapidjson/document.h>

namespace firebase {

class Firebase {
 public:
  Firebase() {}
  virtual ~Firebase() {}

  // Common parameters:
  //   |query_params| - array of params that are joined using the "&" separator
  //       passed verbatim as the query parameter of the request. Can be empty.

  // Retrieves the JSON representation of the data under the given path. See
  // https://firebase.google.com/docs/database/rest/retrieve-data.
  //
  // TODO(ppi): support response Content-Length header, see LE-210.
  virtual void Get(
      const std::string& key,
      const std::vector<std::string>& query_params,
      const std::function<void(Status status, const rapidjson::Value& value)>&
          callback) = 0;

  // Overwrites the data under the given path. Data needs to be a valid JSON
  // object or JSON primitive value.
  // https://firebase.google.com/docs/database/rest/save-data
  virtual void Put(const std::string& key,
                   const std::vector<std::string>& query_params,
                   const std::string& data,
                   const std::function<void(Status status)>& callback) = 0;

  // Adds or updates multiple keys under the given path. Data needs to be a
  // JSON dictionary.
  // https://firebase.google.com/docs/database/rest/save-data
  virtual void Patch(const std::string& key,
                     const std::vector<std::string>& query_params,
                     const std::string& data,
                     const std::function<void(Status status)>& callback) = 0;

  // Deletes the data under the given path.
  virtual void Delete(const std::string& key,
                      const std::vector<std::string>& query_params,
                      const std::function<void(Status status)>& callback) = 0;

  // Registers the given |watch_client| to receive notifications about changes
  // under the given |key|. See
  // https://firebase.google.com/docs/database/rest/retrieve-data.
  virtual void Watch(const std::string& key,
                     const std::vector<std::string>& query_params,
                     WatchClient* watch_client) = 0;

  // Unregisters the given |watch_client|. No calls on the client will be made
  // after this method returns.
  virtual void UnWatch(WatchClient* watch_client) = 0;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(Firebase);
};

}  // namespace firebase

#endif  // APPS_LEDGER_SRC_FIREBASE_FIREBASE_H_
