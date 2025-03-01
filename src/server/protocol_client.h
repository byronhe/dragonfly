// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include <absl/container/inlined_vector.h>

#include <boost/fiber/barrier.hpp>
#include <queue>
#include <variant>

#include "base/io_buf.h"
#include "facade/facade_types.h"
#include "facade/redis_parser.h"
#include "server/common.h"
#include "server/journal/types.h"
#include "server/version.h"
#include "util/fiber_socket_base.h"

namespace facade {
class ReqSerializer;
};  // namespace facade

namespace dfly {

class Service;
class ConnectionContext;
class JournalExecutor;
struct JournalReader;

// A helper class for implementing a Redis client that talks to a redis server.
// This class should be inherited from.
class ProtocolClient {
 public:
  ProtocolClient(std::string master_host, uint16_t port);
  virtual ~ProtocolClient();

  void CloseSocket();  // Close replica sockets.

  uint64_t LastIoTime() const;
  void TouchIoTime();

 protected:
  struct ServerContext {
    std::string host;
    uint16_t port;
    boost::asio::ip::tcp::endpoint endpoint;

    std::string Description() const;
  };

  // Constructing using a fully initialized ServerContext allows to skip
  // the DNS resolution step.
  explicit ProtocolClient(ServerContext context) : server_context_(std::move(context)) {
  }

  std::error_code ResolveMasterDns();  // Resolve master dns
  // Connect to master and authenticate if needed.
  std::error_code ConnectAndAuth(std::chrono::milliseconds connect_timeout_ms, Context* cntx);

  void DefaultErrorHandler(const GenericError& err);

  struct ReadRespRes {
    uint32_t total_read;
    uint32_t left_in_buffer;
  };

  // This function uses parser_ and cmd_args_ in order to consume a single response
  // from the sock_. The output will reside in resp_args_.
  // For error reporting purposes, the parsed command would be in last_resp_ if copy_msg is true.
  // If io_buf is not given, a internal temporary buffer will be used.
  // It is the responsibility of the caller to call buffer->ConsumeInput(rv.left_in_buffer) when it
  // is done with the result of the call; Calling ConsumeInput may invalidate the data in the result
  // if the buffer relocates.
  io::Result<ReadRespRes> ReadRespReply(base::IoBuf* buffer = nullptr, bool copy_msg = true);

  std::error_code ReadLine(base::IoBuf* io_buf, std::string_view* line);

  // Check if reps_args contains a simple reply.
  bool CheckRespIsSimpleReply(std::string_view reply) const;

  // Check resp_args contains the following types at front.
  bool CheckRespFirstTypes(std::initializer_list<facade::RespExpr::Type> types) const;

  // Send command, update last_io_time, return error.
  std::error_code SendCommand(std::string_view command);
  // Send command, read response into resp_args_.
  std::error_code SendCommandAndReadResponse(std::string_view command);

  const ServerContext& server() const {
    return server_context_;
  }

  void ResetParser(bool server_mode);

  auto& LastResponseArgs() {
    return resp_args_;
  }

  auto* Proactor() const {
    return sock_->proactor();
  }

  util::LinuxSocketBase* Sock() const {
    return sock_.get();
  }

 private:
  ServerContext server_context_;

  std::unique_ptr<facade::ReqSerializer> serializer_;
  std::unique_ptr<facade::RedisParser> parser_;
  facade::RespVec resp_args_;
  base::IoBuf resp_buf_;

  std::unique_ptr<util::LinuxSocketBase> sock_;
  Mutex sock_mu_;

 protected:
  Context cntx_;  // context for tasks in replica.

  std::string last_cmd_;
  std::string last_resp_;

  uint64_t last_io_time_ = 0;  // in ns, monotonic clock.
};

}  // namespace dfly

/**
 * A convenience macro to use with ProtocolClient instances for protocol input validation.
 */
#define PC_RETURN_ON_BAD_RESPONSE(x)                                                            \
  do {                                                                                          \
    if (!(x)) {                                                                                 \
      LOG(ERROR) << "Bad response to \"" << last_cmd_ << "\": \"" << absl::CEscape(last_resp_); \
      return std::make_error_code(errc::bad_message);                                           \
    }                                                                                           \
  } while (false)
