// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/firebase/encoding.h"

#include "apps/ledger/src/glue/crypto/base64.h"
#include "lib/ftl/strings/utf_codecs.h"

namespace firebase {

namespace {

// Returns true iff the given value can be put in Firebase without encoding.
// Firebase requires the values to be valid UTF-8 JSON strings. JSON disallows
// control characters in strings. We disallow backslash and double quote to
// avoid reasoning about escaping. Note: this is a stop-gap solution, see
// LE-118.
bool CanValueBeVerbatim(ftl::StringView bytes) {
  // Once encryption is in place this won't be useful. Until then, storing valid
  // utf8 strings verbatim simplifies debugging.
  if (!ftl::IsStringUTF8(bytes)) {
    return false;
  }

  for (const char& byte : bytes) {
    if ((0 <= byte && byte <= 31) || byte == 127 || byte == '\"' ||
        byte == '\\') {
      return false;
    }
  }

  return true;
}

// Characters that are not allowed to appear in a Firebase key (but may appear
// in a value). See
// https://firebase.google.com/docs/database/rest/structure-data.
const char kIllegalKeyChars[] = ".$#[]/+";

// Encodes the given bytes for storage in Firebase. We use the same encoding
// function for both values and keys for simplicity, yielding values that can be
// always safely used as either. Note: this is a stop-gap solution, see LE-118.
std::string Encode(ftl::StringView s, bool verbatim) {
  if (verbatim) {
    return s.ToString() + "V";
  }

  std::string encoded;
  glue::Base64UrlEncode(s, &encoded);
  return encoded + "B";
}

}  // namespace

// Returns true if the given value can be used as a Firebase key without
// encoding.
bool CanKeyBeVerbatim(ftl::StringView bytes) {
  if (!CanValueBeVerbatim(bytes)) {
    return false;
  }

  if (bytes.find_first_of(std::string(kIllegalKeyChars)) != std::string::npos) {
    return false;
  }

  return true;
}

std::string EncodeKey(convert::ExtendedStringView bytes) {
  return Encode(bytes, CanKeyBeVerbatim(bytes));
}

std::string EncodeValue(convert::ExtendedStringView bytes) {
  return Encode(bytes, CanValueBeVerbatim(bytes));
}

bool Decode(convert::ExtendedStringView input, std::string* output) {
  if (input.empty()) {
    return false;
  }

  ftl::StringView data = input.substr(0, input.size() - 1);

  if (input.back() == 'V') {
    *output = data.ToString();
    return true;
  }

  if (input.back() == 'B') {
    return glue::Base64UrlDecode(data, output);
  }

  return false;
}

}  // namespace firebase
