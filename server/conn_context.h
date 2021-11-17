// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include "server/reply_builder.h"

namespace dfly {

class Connection;
class EngineShardSet;

class ConnectionContext : public ReplyBuilder {
 public:
  ConnectionContext(::io::Sink* stream, Connection* owner) : ReplyBuilder(stream), owner_(owner) {
  }

  // TODO: to introduce proper accessors.
  EngineShardSet* shard_set = nullptr;

  Connection* owner() { return owner_;}

 private:
  Connection* owner_;
};

}  // namespace dfly
