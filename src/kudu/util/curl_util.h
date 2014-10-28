// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
#ifndef KUDU_UTIL_CURL_UTIL_H
#define KUDU_UTIL_CURL_UTIL_H

#include <string>

#include "kudu/gutil/macros.h"
#include "kudu/util/status.h"

typedef void CURL;

namespace kudu {

class faststring;

// Simple wrapper around curl's "easy" interface, allowing the user to
// fetch web pages into memory using a blocking API.
//
// This is not thread-safe.
class EasyCurl {
 public:
  EasyCurl();
  ~EasyCurl();

  // Fetch the given URL into the provided buffer.
  // Any existing data in the buffer is replaced.
  Status FetchURL(const std::string& url,
                  faststring* dst);

 private:
  CURL* curl_;
  DISALLOW_COPY_AND_ASSIGN(EasyCurl);
};

} // namespace kudu

#endif /* KUDU_UTIL_CURL_UTIL_H */
