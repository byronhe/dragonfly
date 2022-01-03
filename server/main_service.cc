// Copyright 2021, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/main_service.h"

extern "C" {
  #include "redis/redis_aux.h"
}

#include <absl/strings/ascii.h>
#include <xxhash.h>

#include <boost/fiber/operations.hpp>
#include <filesystem>

#include "base/logging.h"
#include "server/conn_context.h"
#include "server/debugcmd.h"
#include "server/error.h"
#include "server/generic_family.h"
#include "server/list_family.h"
#include "server/string_family.h"
#include "server/transaction.h"
#include "util/metrics/metrics.h"
#include "util/uring/uring_fiber_algo.h"
#include "util/varz.h"

DEFINE_uint32(port, 6380, "Redis port");
DEFINE_uint32(memcache_port, 0, "Memcached port");

namespace dfly {

using namespace std;
using namespace util;
using base::VarzValue;
using ::boost::intrusive_ptr;
namespace fibers = ::boost::fibers;
namespace this_fiber = ::boost::this_fiber;

namespace {

DEFINE_VARZ(VarzMapAverage, request_latency_usec);
DEFINE_VARZ(VarzQps, ping_qps);

std::optional<VarzFunction> engine_varz;
metrics::CounterFamily cmd_req("requests_total", "Number of served redis requests");

constexpr size_t kMaxThreadSize = 1024;

}  // namespace

Service::Service(ProactorPool* pp) : shard_set_(pp), pp_(*pp) {
  CHECK(pp);

  // We support less than 1024 threads.
  CHECK_LT(pp->size(), kMaxThreadSize);
  RegisterCommands();

  engine_varz.emplace("engine", [this] { return GetVarzStats(); });
}

Service::~Service() {
}

void Service::Init(util::AcceptServer* acceptor, const InitOpts& opts) {
  InitRedisTables();

  uint32_t shard_num = pp_.size() > 1 ? pp_.size() - 1 : pp_.size();
  shard_set_.Init(shard_num);

  pp_.AwaitOnAll([&](uint32_t index, ProactorBase* pb) {
    if (index < shard_count()) {
      shard_set_.InitThreadLocal(pb, !opts.disable_time_update);
    }
  });

  request_latency_usec.Init(&pp_);
  ping_qps.Init(&pp_);
  StringFamily::Init(&pp_);
  GenericFamily::Init(&pp_);
  cmd_req.Init(&pp_, {"type"});
}

void Service::Shutdown() {
  VLOG(1) << "Service::Shutdown";

  engine_varz.reset();
  request_latency_usec.Shutdown();
  ping_qps.Shutdown();
  StringFamily::Shutdown();
  GenericFamily::Shutdown();
  cmd_req.Shutdown();
  shard_set_.RunBlockingInParallel([&](EngineShard*) { EngineShard::DestroyThreadLocal(); });
}

void Service::DispatchCommand(CmdArgList args, ConnectionContext* cntx) {
  CHECK(!args.empty());
  DCHECK_NE(0u, shard_set_.size()) << "Init was not called";

  ToUpper(&args[0]);

  VLOG(2) << "Got: " << args;

  string_view cmd_str = ArgS(args, 0);
  const CommandId* cid = registry_.Find(cmd_str);

  if (cid == nullptr) {
    return cntx->SendError(absl::StrCat("unknown command `", cmd_str, "`"));
  }

  if ((cid->arity() > 0 && args.size() != size_t(cid->arity())) ||
      (cid->arity() < 0 && args.size() < size_t(-cid->arity()))) {
    return cntx->SendError(WrongNumArgsError(cmd_str));
  }

  if (cid->key_arg_step() == 2 && (args.size() % 2) == 0) {
    return cntx->SendError(WrongNumArgsError(cmd_str));
  }

  uint64_t start_usec = ProactorBase::GetMonotonicTimeNs(), end_usec;

  // Create command transaction
  intrusive_ptr<Transaction> dist_trans;

  if (cid->first_key_pos() > 0) {
    dist_trans.reset(new Transaction{cid, &shard_set_});
    cntx->transaction = dist_trans.get();

    if (cid->first_key_pos() > 0) {
      dist_trans->InitByArgs(cntx->conn_state.db_index, args);
      cntx->last_command_debug.shards_count = cntx->transaction->unique_shard_cnt();
    }
  } else {
    cntx->transaction = nullptr;
  }

  cntx->cid = cid;
  cmd_req.Inc({cid->name()});
  cid->Invoke(args, cntx);
  end_usec = ProactorBase::GetMonotonicTimeNs();

  request_latency_usec.IncBy(cmd_str, (end_usec - start_usec) / 1000);
  if (dist_trans) {
    cntx->last_command_debug.clock = dist_trans->txid();
  }
}

void Service::DispatchMC(const MemcacheParser::Command& cmd, std::string_view value,
                         ConnectionContext* cntx) {
  absl::InlinedVector<MutableStrSpan, 8> args;
  char cmd_name[16];
  char set_opt[4] = {0};

  switch (cmd.type) {
    case MemcacheParser::REPLACE:
      strcpy(cmd_name, "SET");
      strcpy(set_opt, "XX");
      break;
    case MemcacheParser::SET:
      strcpy(cmd_name, "SET");
      break;
    case MemcacheParser::ADD:
      strcpy(cmd_name, "SET");
      strcpy(set_opt, "NX");
      break;
    case MemcacheParser::GET:
      strcpy(cmd_name, "GET");
      break;
    default:
      cntx->SendMCClientError("bad command line format");
      return;
  }

  args.emplace_back(cmd_name, strlen(cmd_name));
  char* key = const_cast<char*>(cmd.key.data());
  args.emplace_back(key, cmd.key.size());

  if (MemcacheParser::IsStoreCmd(cmd.type)) {
    char* v = const_cast<char*>(value.data());
    args.emplace_back(v, value.size());

    if (set_opt[0]) {
      args.emplace_back(set_opt, strlen(set_opt));
    }
  }

  CmdArgList arg_list{args.data(), args.size()};
  DispatchCommand(arg_list, cntx);
}

void Service::RegisterHttp(HttpListenerBase* listener) {
  CHECK_NOTNULL(listener);
}

void Service::Debug(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[1]);

  DebugCmd dbg_cmd{&shard_set_, cntx};

  return dbg_cmd.Run(args);
}

VarzValue::Map Service::GetVarzStats() {
  VarzValue::Map res;

  atomic_ulong num_keys{0};
  shard_set_.RunBriefInParallel([&](EngineShard* es) { num_keys += es->db_slice().DbSize(0); });
  res.emplace_back("keys", VarzValue::FromInt(num_keys.load()));

  return res;
}

using ServiceFunc = void (Service::*)(CmdArgList args, ConnectionContext* cntx);
inline CommandId::Handler HandlerFunc(Service* se, ServiceFunc f) {
  return [=](CmdArgList args, ConnectionContext* cntx) { return (se->*f)(args, cntx); };
}

#define HFUNC(x) SetHandler(HandlerFunc(this, &Service::x))

void Service::RegisterCommands() {
  using CI = CommandId;

  registry_ << CI{"DEBUG", CO::RANDOM | CO::READONLY, -2, 0, 0, 0}.HFUNC(Debug);

  StringFamily::Register(&registry_);
  GenericFamily::Register(&registry_);
  ListFamily::Register(&registry_);
}

}  // namespace dfly
