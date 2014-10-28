// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
#ifndef KUDU_TWITTER_DEMO_INSERT_CONSUMER_H
#define KUDU_TWITTER_DEMO_INSERT_CONSUMER_H

#include "kudu/twitter-demo/twitter_streamer.h"

#include <string>
#include <tr1/memory>

#include "kudu/client/schema.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/tserver/tserver_service.proxy.h"
#include "kudu/twitter-demo/parser.h"
#include "kudu/util/locks.h"
#include "kudu/util/slice.h"

namespace kudu {
namespace client {
class KuduClient;
class KuduTable;
class KuduSession;
} // namespace client

namespace twitter_demo {

// Consumer of tweet data which parses the JSON and inserts
// into a remote tablet via RPC.
class InsertConsumer : public TwitterConsumer {
 public:
  explicit InsertConsumer(
    const std::tr1::shared_ptr<kudu::client::KuduClient> &client);
  ~InsertConsumer();

  Status Init();

  virtual void ConsumeJSON(const Slice& json) OVERRIDE;

 private:
  void BatchFinished(const Status& s);

  bool initted_;

  client::KuduSchema schema_;

  TwitterEventParser parser_;

  // Reusable object for latest event.
  TwitterEvent event_;

  std::tr1::shared_ptr<client::KuduClient> client_;
  std::tr1::shared_ptr<client::KuduSession> session_;
  scoped_refptr<client::KuduTable> table_;

  simple_spinlock lock_;
  bool request_pending_;
};

} // namespace twitter_demo
} // namespace kudu
#endif
