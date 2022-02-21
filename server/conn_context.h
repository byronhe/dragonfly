// Copyright 2021, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/flat_hash_set.h>

#include "server/common_types.h"
#include "server/reply_builder.h"

namespace dfly {

class Connection;
class EngineShardSet;

struct StoredCmd {
  const CommandId* descr;
  std::vector<std::string> cmd;

  StoredCmd(const CommandId* d = nullptr) : descr(d) {
  }
};

struct ConnectionState {
  DbIndex db_index = 0;

  enum ExecState { EXEC_INACTIVE, EXEC_COLLECT, EXEC_ERROR };

  ExecState exec_state = EXEC_INACTIVE;
  std::vector<StoredCmd> exec_body;

  enum Mask : uint32_t {
    ASYNC_DISPATCH = 1,  // whether a command is handled via async dispatch.
    CONN_CLOSING = 2,    // could be because of unrecoverable error or planned action.

    // Whether this connection belongs to replica, i.e. a dragonfly slave is connected to this
    // host (master) via this connection to sync from it.
    REPL_CONNECTION = 4,
    REQ_AUTH = 8,
    AUTHENTICATED = 0x10,
  };

  uint32_t mask = 0;  // A bitmask of Mask values.

  enum MCGetMask {
    FETCH_CAS_VER = 1,
  };

  // used for memcache set/get commands.
  // For set op - it's the flag value we are storing along with the value.
  // For get op - we use it as a mask of MCGetMask values.
  uint32_t memcache_flag = 0;

  bool IsClosing() const {
    return mask & CONN_CLOSING;
  }

  bool IsRunViaDispatch() const {
    return mask & ASYNC_DISPATCH;
  }

  // Lua-script related data.
  struct Script {
    bool is_write = true;

    absl::flat_hash_set<std::string_view> keys;
  };
  std::optional<Script> script_info;
};

class ConnectionContext {
 public:
  ConnectionContext(::io::Sink* stream, Connection* owner);

  struct DebugInfo {
    uint32_t shards_count = 0;
    TxClock clock = 0;
    bool is_ooo = false;
  };

  DebugInfo last_command_debug;

  // TODO: to introduce proper accessors.
  Transaction* transaction = nullptr;
  const CommandId* cid = nullptr;
  EngineShardSet* shard_set = nullptr;

  Connection* owner() {
    return owner_;
  }

  Protocol protocol() const;

  DbIndex db_index() const {
    return conn_state.db_index;
  }

  ConnectionState conn_state;

  // A convenient proxy for redis interface.
  RedisReplyBuilder* operator->();

  ReplyBuilderInterface* reply_builder() {
    return rbuilder_.get();
  }

  // Allows receiving the output data from the commands called from scripts.
  ReplyBuilderInterface* Inject(ReplyBuilderInterface* new_i) {
    ReplyBuilderInterface* res = rbuilder_.release();
    rbuilder_.reset(new_i);
    return res;
  }

 private:
  Connection* owner_;
  std::unique_ptr<ReplyBuilderInterface> rbuilder_;
};

}  // namespace dfly
