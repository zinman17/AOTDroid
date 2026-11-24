// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/binder_emu.cpp   (MODULE 3)
// ============================================================================
#include "binder_emu.h"
#include "vfs.h"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

namespace aotdroid {

// ---------------------------------------------------------------------------
// ioctl encodings (mirror of uapi/linux/android/binder.h)
// ---------------------------------------------------------------------------
const uint32_t kBINDER_WRITE_READ =
    (uint32_t)_IOWR('b', 1, struct binder_write_read);
const uint32_t kBINDER_SET_MAX_THREADS = (uint32_t)_IOW('b', 5, size_t);
const uint32_t kBINDER_SET_CONTEXT_MGR = (uint32_t)_IO('b', 7);
const uint32_t kBINDER_SET_CONTEXT_MGR_EXT =
    (uint32_t)_IOW('b', 17, struct flat_binder_object);
const uint32_t kBINDER_THREAD_EXIT = (uint32_t)_IOW('b', 8, int);
const uint32_t kBINDER_VERSION = (uint32_t)_IOWR('b', 9, struct binder_version);
const uint32_t kBINDER_GET_NODE_DEBUG_INFO =
    (uint32_t)_IOWR('b', 11, struct binder_node_debug_info);
const uint32_t kBINDER_ENABLE_ONEWAY_SPAM_DETECTION =
    (uint32_t)_IOW('b', 20, uint32_t);

uint32_t g_bc[kBinderCmdCount] = {};
uint32_t g_br[kBinderCmdCount] = {};
namespace {
struct CmdTableInit {
  CmdTableInit() {
    g_bc[BC_TRANSACTION_] = (uint32_t)_IOW('c', 0, struct binder_transaction_data);
    g_bc[BC_REPLY_]       = (uint32_t)_IOW('c', 1, struct binder_transaction_data);
    g_bc[BC_ACQUIRE_RESULT] = (uint32_t)_IOW('c', 2, int32_t);
    g_bc[BC_FREE_BUFFER_] = (uint32_t)_IOW('c', 3, uint64_t);
    g_bc[BC_INCREFS]      = (uint32_t)_IOW('c', 4, struct binder_ptr_cookie);
    g_bc[BC_ACQUIRE]      = (uint32_t)_IOW('c', 5, struct binder_ptr_cookie);
    g_bc[BC_RELEASE]      = (uint32_t)_IOW('c', 6, struct binder_ptr_cookie);
    g_bc[BC_DECREFS]      = (uint32_t)_IOW('c', 7, struct binder_ptr_cookie);
    g_bc[BC_INCREFS_DONE] = (uint32_t)_IOW('c', 8, struct binder_ptr_cookie);
    g_bc[BC_ACQUIRE_DONE] = (uint32_t)_IOW('c', 9, struct binder_ptr_cookie);
    g_bc[BC_ATTEMPT_ACQUIRE] = (uint32_t)_IOW('c', 10, struct binder_pri_desc);
    g_bc[BC_REQUEST_DEATH_NOTIFICATION] = (uint32_t)_IOW('c', 11, struct binder_ptr_cookie);
    g_bc[BC_CLEAR_DEATH_NOTIFICATION]   = (uint32_t)_IOW('c', 12, struct binder_ptr_cookie);
    g_bc[BC_DEAD_BINDER_DONE] = (uint32_t)_IOW('c', 13, uint64_t);
    g_bc[BC_TRANSACTION_SG] = (uint32_t)_IOW('c', 14, struct binder_transaction_data);
    g_bc[BC_REPLY_SG]       = (uint32_t)_IOW('c', 15, struct binder_transaction_data);
    g_bc[BC_ENTER_LOOPER]    = (uint32_t)_IO('c', 16);
    g_bc[BC_REGISTER_LOOPER] = (uint32_t)_IO('c', 17);
    g_bc[BC_EXIT_LOOPER]     = (uint32_t)_IO('c', 18);

    g_br[BR_ERROR_] = (uint32_t)_IOR('r', 0, int32_t);
    g_br[BR_OK]     = (uint32_t)_IO('r', 1);
    g_br[BR_TRANSACTION_] = (uint32_t)_IOR('r', 2, struct binder_transaction_data);
    g_br[BR_REPLY_]       = (uint32_t)_IOR('r', 3, struct binder_transaction_data);
    g_br[BR_DEAD_REPLY]   = (uint32_t)_IO('r', 5);
    g_br[BR_TRANSACTION_COMPLETE] = (uint32_t)_IO('r', 6);
    g_br[BR_INCREFS] = (uint32_t)_IOR('r', 7, struct binder_ptr_cookie);
    g_br[BR_ACQUIRE] = (uint32_t)_IOR('r', 8, struct binder_ptr_cookie);
    g_br[BR_RELEASE] = (uint32_t)_IOR('r', 9, struct binder_ptr_cookie);
    g_br[BR_DECREFS] = (uint32_t)_IOR('r', 10, struct binder_ptr_cookie);
    g_br[BR_ATTEMPT_ACQUIRE] = (uint32_t)_IOR('r', 11, struct binder_pri_ptr_cookie);
    g_br[BR_NOOP]     = (uint32_t)_IO('r', 12);
    g_br[BR_SPAWN_LOOPER] = (uint32_t)_IO('r', 13);
    g_br[BR_DEAD_BINDER] = (uint32_t)_IOR('r', 15, uint64_t);
    g_br[BR_CLEAR_DEATH_NOTIFICATION_DONE] = (uint32_t)_IOR('r', 16, uint64_t);
    g_br[BR_FAILED_REPLY] = (uint32_t)_IO('r', 17);
  }
} g_cmdInit;
}  // namespace

// ---------------------------------------------------------------------------
// Domain internals
// ---------------------------------------------------------------------------
namespace {

struct ReplyWaiter {
  std::mutex mu;
  std::condition_variable cv;
  bool ready = false, failed = false;
  std::vector<uint32_t> br;
};

struct WorkItem {
  DomainId targetDomain;
  std::vector<uint32_t> cmds;      // BR stream
  uint64_t waiterId = 0;           // nonzero for sync txn deliveries
};

struct Domain {
  int which = 0;
  uint64_t contextMgrPtr = 0;
  DomainId contextMgrOwner = kInvalidDomain;
  uint32_t maxThreads = 15;

  std::unordered_map<uint64_t, BinderNode> nodes;
  struct Ref { uint64_t nodePtr; int32_t strong, weak; };
  std::unordered_map<DomainId, std::unordered_map<uint32_t, Ref>> refs;
  std::unordered_map<DomainId, std::unordered_map<uint64_t, uint32_t>> descByNode;
  std::unordered_map<DomainId, uint32_t> nextDesc;

  std::mutex mu;
  std::condition_variable cv;
  std::deque<WorkItem> work;
  std::unordered_map<uint64_t, std::shared_ptr<ReplyWaiter>> waiters;

  struct BufRec { DomainId owner; std::vector<uint8_t> data;
                  std::vector<uint64_t> offsets; };
  std::unordered_map<uint64_t, std::unique_ptr<BufRec>> liveBuffers;
  std::unordered_map<uint64_t, std::vector<std::pair<DomainId, uint64_t>>> deathSubs;

  uint32_t internRef(DomainId owner, uint64_t nodePtr) {
    uint32_t& next = nextDesc[owner];
    auto& byNode = descByNode[owner];
    auto it = byNode.find(nodePtr);
    if (it != byNode.end()) return it->second;
    uint32_t desc = next++;
    if (desc == 0) desc = next++;
    // context manager is always handle 0
    if (nodePtr == contextMgrPtr) desc = 0;
    byNode[nodePtr] = desc;
    refs[owner][desc] = Ref{nodePtr, 0, 0};
    return desc;
  }
  Ref* getRef(DomainId owner, uint32_t desc) {
    auto it = refs.find(owner);
    if (it == refs.end()) return nullptr;
    auto r = it->second.find(desc);
    return r == it->second.end() ? nullptr : &r->second;
  }
};

struct ThrState {
  DomainId domain = kInvalidDomain;
  int which = -1;
  uint32_t looper = 0;
  std::vector<uint64_t> txnStack;   // outstanding deliveries (waiter ids)
  std::vector<uint64_t> myWaiters;  // waiter ids this thread is blocked on
};
ThrState& thr() { static thread_local ThrState t; return t; }

void pushCmd(std::vector<uint32_t>* v, uint32_t cmd) { v->push_back(cmd); }
template <typename T>
void pushCmd(std::vector<uint32_t>* v, uint32_t cmd, const T& payload) {
  v->push_back(cmd);
  const uint32_t* p = (const uint32_t*)&payload;
  for (size_t i = 0; i < (sizeof(T) + 3) / 4; ++i) v->push_back(p[i]);
}
std::vector<uint32_t> encodeTxnBR(uint32_t brCmd, const binder_transaction_data& td) {
  std::vector<uint32_t> v;
  pushCmd(&v, brCmd, td);
  return v;
}
bool translateObjects(Domain* c, DomainId from, DomainId to, uint8_t* data,
                      size_t dataLen, const uint64_t* offsets, size_t offsetsLen,
                      std::string* err) {
  size_t n = offsetsLen / 8;
  for (size_t i = 0; i < n; ++i) {
    if (offsets[i] + sizeof(flat_binder_object) > dataLen) {
      *err = "flat_binder_object oob"; return false;
    }
    flat_binder_object* o = (flat_binder_object*)(data + offsets[i]);
    switch (o->type) {
      case kBINDER_TYPE_BINDER:
      case kBINDER_TYPE_WEAK_BINDER: {
        BinderNode& node = c->nodes[o->binder];
        if (node.ptr == 0) { node.ptr = o->binder; node.cookie = o->cookie;
                             node.owner = from; }
        uint32_t desc = c->internRef(to, o->binder);
        o->type = (o->type == kBINDER_TYPE_BINDER) ? kBINDER_TYPE_HANDLE
                                                   : kBINDER_TYPE_WEAK_HANDLE;
        o->binder = desc; o->cookie = 0;
        break;
      }
      case kBINDER_TYPE_HANDLE:
      case kBINDER_TYPE_WEAK_HANDLE: {
        auto* r = c->getRef(from, (uint32_t)o->binder);
        if (!r) { *err = "bad handle"; return false; }
        BinderNode& node = c->nodes[r->nodePtr];
        if (node.owner == to) {
          o->type = (o->type == kBINDER_TYPE_HANDLE) ? kBINDER_TYPE_BINDER
                                                     : kBINDER_TYPE_WEAK_BINDER;
          o->binder = node.ptr; o->cookie = node.cookie;
        } else {
          o->binder = c->internRef(to, r->nodePtr);
        }
        break;
      }
      case kBINDER_TYPE_FD: {
        int nfd = fds().dup((int)o->binder);
        if (nfd < 0) { *err = "fd dup"; return false; }
        o->binder = (uint64_t)(uint32_t)nfd;
        break;
      }
      default: break;
    }
  }
  return true;
}
}  // namespace

// header-level BinderDomain is opaque; keep the real storage here

class BinderEmuImpl {
 public:
  Domain* ctx[3] = {nullptr, nullptr, nullptr};
  uint32_t proto = 7;
  std::atomic<uint64_t> nextId{1};
  std::atomic<uint64_t> txns{0};
  std::mutex gmu;
};
static BinderEmuImpl& impl() { static BinderEmuImpl i; return i; }

// ---------------------------------------------------------------------------
BinderEmu& BinderEmu::get() { static BinderEmu e; return e; }

void BinderEmu::configure(uint32_t androidApi) {
  switch (androidApi) {
    case 25: impl().proto = 6; break;
    case 27: impl().proto = 7; break;
    case 29: impl().proto = 8; break;
    default: impl().proto = 7; break;
  }
}
uint32_t BinderEmu::protocolVersion() const { return impl().proto; }

void BinderEmu::initContexts() {
  auto& m = impl();
  std::lock_guard<std::mutex> lk(m.gmu);
  for (int i = 0; i < 3; ++i)
    if (!m.ctx[i]) { m.ctx[i] = new Domain(); m.ctx[i]->which = i; }
}
BinderDomain* BinderEmu::context(int which) {
  if (which < 0 || which > 2) return nullptr;
  return (BinderDomain*)impl().ctx[which];   // opaque handle; used by tests
}
uint64_t BinderEmu::contextManagerNode(int which) const {
  auto* c = impl().ctx[which];
  return c ? c->contextMgrPtr : 0;
}
size_t BinderEmu::nodesAlive() const {
  size_t n = 0;
  for (auto* c : impl().ctx) if (c) n += c->nodes.size();
  return n;
}

// ---------------------------------------------------------------------------
// ioctl(BINDER_WRITE_READ) + friends
// ---------------------------------------------------------------------------
long BinderEmu::ioctl(int which, uint32_t cmd, void* arg, DomainId caller) {
  auto& m = impl();
  if (which < 0 || which > 2 || !m.ctx[which]) return -EBADF;
  Domain* c = m.ctx[which];
  ThrState& ts = thr();
  if (ts.domain == kInvalidDomain) { ts.domain = caller; ts.which = which; }

  if (cmd == kBINDER_VERSION) {
    if (!arg) return -EINVAL;
    ((binder_version*)arg)->protocol_version = (int32_t)m.proto;
    return 0;
  }
  if (cmd == kBINDER_SET_MAX_THREADS) { c->maxThreads = *(uint32_t*)arg; return 0; }
  if (cmd == kBINDER_SET_CONTEXT_MGR || cmd == kBINDER_SET_CONTEXT_MGR_EXT) {
    std::lock_guard<std::mutex> lk(c->mu);
    if (c->contextMgrPtr != 0) return -EBUSY;
    if (cmd == kBINDER_SET_CONTEXT_MGR_EXT) {
      auto* fbo = (flat_binder_object*)arg;
      c->contextMgrPtr = fbo->binder;
      BinderNode& n = c->nodes[fbo->binder];
      n.ptr = fbo->binder; n.cookie = fbo->cookie; n.owner = caller;
    } else {
      c->contextMgrPtr = 1;
      BinderNode& n = c->nodes[1];
      n.ptr = 1; n.cookie = 0; n.owner = caller;
    }
    c->contextMgrOwner = caller;
    c->descByNode[caller][c->contextMgrPtr] = 0;
    c->refs[caller][0] = {c->contextMgrPtr, 0, 0};
    return 0;
  }
  if (cmd == kBINDER_THREAD_EXIT) { ts.looper = 0; return 0; }
  if (cmd == kBINDER_GET_NODE_DEBUG_INFO) { memset(arg, 0, sizeof(binder_node_debug_info)); return 0; }
  if (cmd == kBINDER_ENABLE_ONEWAY_SPAM_DETECTION) return 0;
  if (cmd != kBINDER_WRITE_READ) return -ENOTTY;

  auto* bwr = (binder_write_read*)arg;

  // ---- write side ------------------------------------------------------
  std::vector<uint32_t> immediateBR;
  auto bcPayloadSize = [](uint32_t bc) -> long {
    if (bc == g_bc[BC_TRANSACTION_] || bc == g_bc[BC_REPLY_] ||
        bc == g_bc[BC_TRANSACTION_SG] || bc == g_bc[BC_REPLY_SG])
      return (long)sizeof(binder_transaction_data);
    if (bc == g_bc[BC_INCREFS] || bc == g_bc[BC_ACQUIRE] ||
        bc == g_bc[BC_RELEASE] || bc == g_bc[BC_DECREFS] ||
        bc == g_bc[BC_INCREFS_DONE] || bc == g_bc[BC_ACQUIRE_DONE] ||
        bc == g_bc[BC_REQUEST_DEATH_NOTIFICATION] ||
        bc == g_bc[BC_CLEAR_DEATH_NOTIFICATION])
      return (long)sizeof(binder_ptr_cookie);
    if (bc == g_bc[BC_FREE_BUFFER_] || bc == g_bc[BC_DEAD_BINDER_DONE])
      return (long)sizeof(uint64_t);
    if (bc == g_bc[BC_ENTER_LOOPER] || bc == g_bc[BC_REGISTER_LOOPER] ||
        bc == g_bc[BC_EXIT_LOOPER])
      return 0;
    return -1;
  };
  uint64_t wpos = 0;
  while (wpos + 4 <= bwr->write_size) {
    uint32_t bc; memcpy(&bc, (void*)(uintptr_t)(bwr->write_buffer + wpos), 4);
    long psz = bcPayloadSize(bc);
    if (psz < 0 || wpos + 4 + (uint64_t)psz > bwr->write_size) return -EINVAL;
    const uint8_t* pl = (const uint8_t*)(uintptr_t)bwr->write_buffer + wpos + 4;
    wpos += 4 + (uint64_t)psz;

    if (bc == g_bc[BC_ENTER_LOOPER]) { ts.looper |= 1; continue; }
    if (bc == g_bc[BC_REGISTER_LOOPER]) { ts.looper |= 2; continue; }
    if (bc == g_bc[BC_EXIT_LOOPER]) { ts.looper &= ~3u; continue; }
    if (bc == g_bc[BC_FREE_BUFFER_]) {
      uint64_t buf; memcpy(&buf, pl, 8);
      std::lock_guard<std::mutex> lk(c->mu);
      c->liveBuffers.erase(buf);
      continue;
    }
    if (bc == g_bc[BC_INCREFS] || bc == g_bc[BC_ACQUIRE] ||
        bc == g_bc[BC_RELEASE] || bc == g_bc[BC_DECREFS]) {
      binder_ptr_cookie pc; memcpy(&pc, pl, sizeof(pc));
      auto* r = c->getRef(caller, (uint32_t)pc.ptr);
      if (!r) continue;
      BinderNode& node = c->nodes[r->nodePtr];
      bool inc = (bc == g_bc[BC_INCREFS] || bc == g_bc[BC_ACQUIRE]);
      bool strong = (bc == g_bc[BC_ACQUIRE] || bc == g_bc[BC_RELEASE]);
      if (strong) r->strong += inc ? 1 : -1; else r->weak += inc ? 1 : -1;
      if (node.owner != caller && node.owner != kInvalidDomain) {
        std::vector<uint32_t> br;
        pushCmd(&br, strong ? g_br[BR_ACQUIRE] : g_br[BR_INCREFS],
                binder_ptr_cookie{node.ptr, node.cookie});
        {
          std::lock_guard<std::mutex> lk(c->mu);
          c->work.push_back(WorkItem{node.owner, std::move(br), 0});
          c->cv.notify_all();
        }
      }
      continue;
    }
    if (bc == g_bc[BC_INCREFS_DONE] || bc == g_bc[BC_ACQUIRE_DONE]) {
      binder_ptr_cookie pc; memcpy(&pc, pl, sizeof(pc));
      auto it = c->nodes.find(pc.ptr);
      if (it != c->nodes.end()) {
        if (bc == g_bc[BC_ACQUIRE_DONE]) it->second.strong--;
        else it->second.weak--;
      }
      continue;
    }
    if (bc == g_bc[BC_REQUEST_DEATH_NOTIFICATION] ||
        bc == g_bc[BC_CLEAR_DEATH_NOTIFICATION]) {
      binder_ptr_cookie pc; memcpy(&pc, pl, sizeof(pc));
      auto* r = c->getRef(caller, (uint32_t)pc.ptr);
      if (!r) continue;
      if (bc == g_bc[BC_REQUEST_DEATH_NOTIFICATION]) {
        std::lock_guard<std::mutex> lk(c->mu);
        c->deathSubs[r->nodePtr].push_back({caller, pc.cookie});
      } else {
        std::lock_guard<std::mutex> lk(c->mu);
        auto& v = c->deathSubs[r->nodePtr];
        for (size_t i = 0; i < v.size(); ) {
          if (v[i].first == caller && v[i].second == pc.cookie) {
            std::swap(v[i], v.back()); v.pop_back();
          } else ++i;
        }
      }
      continue;
    }
    if (bc == g_bc[BC_TRANSACTION_] || bc == g_bc[BC_REPLY_]) {
      binder_transaction_data td; memcpy(&td, pl, sizeof(td));
      bool isReply = (bc == g_bc[BC_REPLY_]);
      const uint8_t* srcData = td.data_size <= sizeof(td.data.buf)
        ? td.data.buf : (const uint8_t*)(uintptr_t)td.data.ptr.buffer;
      std::vector<uint64_t> offs;
      if (td.data_size > sizeof(td.data.buf)) {
        const uint64_t* so = (const uint64_t*)(uintptr_t)td.data.ptr.offsets;
        offs.assign(so, so + td.offsets_size / 8);
      }
      auto buf = std::make_unique<Domain::BufRec>();
      buf->owner = caller;
      buf->data.assign(srcData, srcData + td.data_size);
      buf->offsets = offs;

      if (isReply) {
        // reply to the thread that issued the pending sync transaction
        if (ts.txnStack.empty()) {
          pushCmd(&immediateBR, g_br[BR_FAILED_REPLY]);
          break;
        }
        uint64_t wid = ts.txnStack.back();
        ts.txnStack.pop_back();
        std::shared_ptr<ReplyWaiter> waiter;
        {
          std::lock_guard<std::mutex> lk(c->mu);
          auto wit = c->waiters.find(wid);
          if (wit == c->waiters.end()) { pushCmd(&immediateBR, g_br[BR_FAILED_REPLY]); break; }
          waiter = wit->second;
          // NOTE: do NOT erase here -- the WAITING thread owns the entry so a
          // reply that lands before the caller enters its wait is never lost.
        }
        if (!offs.empty()) {
          std::string terr;
          if (!translateObjects(c, caller, ts.domain, buf->data.data(),
                                buf->data.size(), offs.data(), offs.size() * 8, &terr)) {
            pushCmd(&immediateBR, g_br[BR_FAILED_REPLY]);
            break;
          }
        }
        binder_transaction_data rtd{};
        rtd.code = td.code;
        rtd.sender_pid = (int32_t)caller;
        rtd.data_size = buf->data.size();
        rtd.offsets_size = offs.size() * 8;
        // Single process: hand the recipient the REAL buffer/offsets
        // pointers (kernel addresses in real binder); BC_FREE_BUFFER returns
        // them through the liveBuffers registry.
        uint64_t bufPtr = (uint64_t)(uintptr_t)buf->data.data();
        uint64_t offPtr = (uint64_t)(uintptr_t)buf->offsets.data();
        rtd.data.ptr.buffer = bufPtr;
        rtd.data.ptr.offsets = offPtr;
        {
          std::lock_guard<std::mutex> lk(c->mu);
          c->liveBuffers[bufPtr] = std::move(buf);
        }
        {
          std::lock_guard<std::mutex> wl(waiter->mu);
          waiter->br = encodeTxnBR(g_br[BR_REPLY_], rtd);
          waiter->ready = true;
          waiter->cv.notify_all();
        }
        m.txns++;
        continue;
      }
      // BC_TRANSACTION
      uint64_t nodePtr;
      if (td.target.handle == 0) {
        nodePtr = c->contextMgrPtr;
        if (nodePtr == 0) { pushCmd(&immediateBR, g_br[BR_DEAD_REPLY]); break; }
      } else {
        auto* r = c->getRef(caller, td.target.handle);
        if (!r) { pushCmd(&immediateBR, g_br[BR_FAILED_REPLY]); break; }
        nodePtr = r->nodePtr;
      }
      BinderNode& node = c->nodes[nodePtr];
      if (!offs.empty()) {
        std::string terr;
        if (!translateObjects(c, caller, node.owner, buf->data.data(),
                              buf->data.size(), offs.data(), offs.size() * 8, &terr)) {
          pushCmd(&immediateBR, g_br[BR_FAILED_REPLY]);
          break;
        }
      }
      uint64_t bufPtr = (uint64_t)(uintptr_t)buf->data.data();
      uint64_t offPtr = (uint64_t)(uintptr_t)buf->offsets.data();
      binder_transaction_data rtd{};
      rtd.target.ptr = nodePtr;
      rtd.cookie = node.cookie;
      rtd.code = td.code; rtd.flags = td.flags;
      rtd.sender_pid = (int32_t)caller;
      rtd.data_size = buf->data.size();
      rtd.offsets_size = offs.size() * 8;
      rtd.data.ptr.buffer = bufPtr;
      rtd.data.ptr.offsets = offPtr;
      {
        std::lock_guard<std::mutex> lk(c->mu);
        c->liveBuffers[bufPtr] = std::move(buf);
      }
      uint64_t waiterId = 0;
      if (!(td.flags & kTF_ONE_WAY)) {
        waiterId = m.nextId.fetch_add(1);
        auto waiter = std::make_shared<ReplyWaiter>();
        std::lock_guard<std::mutex> lk(c->mu);
        c->waiters[waiterId] = waiter;
        ts.myWaiters.push_back(waiterId);
      }
      {
        std::lock_guard<std::mutex> lk(c->mu);
        c->work.push_back(WorkItem{node.owner,
                                   encodeTxnBR(g_br[BR_TRANSACTION_], rtd),
                                   waiterId});
        c->cv.notify_all();
      }
      m.txns++;
      if (!(td.flags & kTF_ONE_WAY))
        pushCmd(&immediateBR, g_br[BR_TRANSACTION_COMPLETE]);
      continue;
    }
    return -EINVAL;
  }
  bwr->write_consumed = wpos;

  // ---- read side -------------------------------------------------------
  if (bwr->read_size == 0) return 0;
  std::vector<uint32_t> out = std::move(immediateBR);

  // drain queued work for this domain (recording txn stack entries)
  auto drainWork = [&]() {
    std::lock_guard<std::mutex> lk(c->mu);
    while (!c->work.empty() && out.size() * 4 + 64 <= bwr->read_size) {
      WorkItem& w = c->work.front();
      if (w.targetDomain != ts.domain) {
        // keep others' work: try next item
        bool advanced = false;
        for (auto it2 = c->work.begin() + 1; it2 != c->work.end(); ++it2) {
          if (it2->targetDomain == ts.domain) {
            for (uint32_t x : it2->cmds) out.push_back(x);
            if (it2->waiterId) ts.txnStack.push_back(it2->waiterId);
            c->work.erase(it2);
            advanced = true;
            break;
          }
        }
        if (!advanced) break;
        continue;
      }
      for (uint32_t x : w.cmds) out.push_back(x);
      if (w.waiterId) ts.txnStack.push_back(w.waiterId);
      c->work.pop_front();
    }
  };
  drainWork();

  if (out.empty()) {
    // wait for a reply to one of MY outstanding sync txns first
    std::shared_ptr<ReplyWaiter> waiter;
    {
      std::lock_guard<std::mutex> lk(c->mu);
      for (uint64_t wid : ts.myWaiters) {
        auto it = c->waiters.find(wid);
        if (it != c->waiters.end()) { waiter = it->second; break; }
      }
    }
    if (waiter) {
      std::unique_lock<std::mutex> wl(waiter->mu);
      waiter->cv.wait_for(wl, std::chrono::seconds(30),
                          [&] { return waiter->ready || waiter->failed; });
      bool ok = waiter->ready;
      auto br = std::move(waiter->br);
      wl.unlock();
      {
        std::lock_guard<std::mutex> lk(c->mu);
        for (auto it = ts.myWaiters.begin(); it != ts.myWaiters.end(); ++it) {
          auto w2 = c->waiters.find(*it);
          if (w2 != c->waiters.end() && w2->second == waiter) { c->waiters.erase(w2); break; }
        }
        // also purge the txnStack copy if the replier pushed it there
        ts.myWaiters.clear();
      }
      if (ok) out = std::move(br);
      else out.push_back(g_br[BR_FAILED_REPLY]);
    } else {
      std::unique_lock<std::mutex> lk(c->mu);
      bool have = c->cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
        for (auto& w : c->work)
          if (w.targetDomain == ts.domain) return true;
        return false;
      });
      if (!have) out.push_back(g_br[BR_NOOP]);
      lk.unlock();
      drainWork();
    }
  }

  size_t bytes = std::min<uint64_t>(out.size() * 4, bwr->read_size & ~3ull);
  if (bytes) memcpy((void*)(uintptr_t)bwr->read_buffer, out.data(), bytes);
  bwr->read_consumed = bytes;
  return 0;
}

// ---------------------------------------------------------------------------
void BinderEmu::domainExit(DomainId d) {
  auto& m = impl();
  for (auto* c : m.ctx) {
    if (!c) continue;
    std::vector<std::pair<DomainId, uint64_t>> notify;
    {
      std::lock_guard<std::mutex> lk(c->mu);
      for (auto& kv : c->nodes)
        if (kv.second.owner == d) kv.second.dead = true;
      for (auto& kv : c->refs) {
        for (auto& kv2 : kv.second) {
          auto nit = c->nodes.find(kv2.second.nodePtr);
          if (nit != c->nodes.end() && nit->second.dead) kv2.second.strong = INT32_MIN;
        }
      }
      for (auto& kv : c->deathSubs) {
        if (c->nodes[kv.first].dead)
          for (auto& sub : kv.second) notify.push_back(sub);
      }
      for (auto& n : notify) {
        std::vector<uint32_t> br;
        br.push_back(g_br[BR_DEAD_BINDER]);
        br.push_back((uint32_t)n.second);
        br.push_back((uint32_t)(n.second >> 32));
        c->work.push_back(WorkItem{n.first, std::move(br), 0});
      }
      c->cv.notify_all();
    }
  }
}

bool BinderEmu::enqueueTransaction(int which, DomainId targetDomain,
                                   uint32_t targetHandle, uint32_t code,
                                   uint32_t flags, const uint8_t* data,
                                   size_t dataLen, const uint64_t* offsets,
                                   size_t offsetsLen, int32_t senderPid,
                                   uint32_t senderEuid, uint64_t* outReplyId) {
  auto& m = impl();
  Domain* c = m.ctx[which];
  if (!c) return false;
  uint64_t nodePtr;
  if (targetHandle == 0) nodePtr = c->contextMgrPtr;
  else {
    auto* r = c->getRef(targetDomain, targetHandle);
    if (!r) return false;
    nodePtr = r->nodePtr;
  }
  BinderNode& node = c->nodes[nodePtr];
  auto buf = std::make_unique<Domain::BufRec>();
  buf->owner = targetDomain;
  buf->data.assign(data, data + dataLen);
  buf->offsets.assign(offsets, offsets + offsetsLen / 8);
  uint64_t bufPtr = (uint64_t)(uintptr_t)buf->data.data();
  uint64_t offPtr = (uint64_t)(uintptr_t)buf->offsets.data();
  binder_transaction_data rtd{};
  rtd.target.ptr = nodePtr;
  rtd.cookie = node.cookie;
  rtd.code = code; rtd.flags = flags;
  rtd.sender_pid = senderPid; rtd.sender_euid = senderEuid;
  rtd.data_size = dataLen; rtd.offsets_size = offsetsLen;
  rtd.data.ptr.buffer = bufPtr; rtd.data.ptr.offsets = offPtr;
  {
    std::lock_guard<std::mutex> lk(c->mu);
    c->liveBuffers[bufPtr] = std::move(buf);
    c->work.push_back(WorkItem{node.owner, encodeTxnBR(g_br[BR_TRANSACTION_], rtd), 0});
    c->cv.notify_all();
  }
  if (outReplyId) *outReplyId = bufPtr;
  m.txns++;
  return true;
}

// ---------------------------------------------------------------------------
// /dev/ashmem emulation
// ---------------------------------------------------------------------------
const uint32_t kASHMEM_SET_NAME = (uint32_t)_IOW(0x77, 1, char[256]);
const uint32_t kASHMEM_GET_NAME = (uint32_t)_IOR(0x77, 2, char[256]);
const uint32_t kASHMEM_SET_SIZE = (uint32_t)_IOW(0x77, 3, size_t);
const uint32_t kASHMEM_GET_SIZE = (uint32_t)_IO(0x77, 4);
const uint32_t kASHMEM_SET_PROT_MASK = (uint32_t)_IOW(0x77, 5, uint32_t);
const uint32_t kASHMEM_PIN = (uint32_t)_IOW(0x77, 77, AshmemPin);
const uint32_t kASHMEM_UNPIN = (uint32_t)_IOW(0x77, 78, AshmemPin);
const uint32_t kASHMEM_GET_PIN_STATUS = (uint32_t)_IO(0x77, 79);
const uint32_t kASHMEM_PURGE_ALL_CACHES = (uint32_t)_IO(0x77, 80);

AshmemEmu& AshmemEmu::get() { static AshmemEmu a; return a; }

int AshmemEmu::create(const char* name, size_t size) {
  auto vn = std::make_shared<VNode>();
  vn->kind = VNKind::Ashmem;
  vn->name = name ? name : "aotdroid-ashmem";
  int fd = fds().alloc(vn);
  Area a;
  a.name = vn->name;
  a.size = size;
#ifdef AOTDROID_APPLE
  char shmName[64];
  snprintf(shmName, sizeof(shmName), "aotdroid.ashmem.%u", nextId_++);
  int sh = shm_open(shmName, O_CREAT | O_RDWR, 0600);
  if (sh < 0) return -1;
  if (ftruncate(sh, (off_t)size) != 0) { ::close(sh); return -1; }
  a.shmHost = sh;
  a.map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, sh, 0);
  if (a.map == MAP_FAILED) { ::close(sh); a.map = nullptr; return -1; }
#else
  a.map = calloc(1, size ? size : 1);
#endif
  a.pinned = size;   // ashmem areas start fully pinned
  {
    std::lock_guard<std::mutex> lk(mu_);
    areas_[vn.get()] = std::move(a);
  }
  pinned_ += size;
  return fd;
}

long AshmemEmu::ioctl(VNode* vn, uint32_t cmd, void* arg) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = areas_.find(vn);
  if (it == areas_.end()) return -ENOENT;
  Area& a = it->second;
  if (cmd == kASHMEM_SET_NAME) {
    a.name = (const char*)arg;
    return 0;
  }
  if (cmd == kASHMEM_GET_NAME) {
    snprintf((char*)arg, kAshmemNameLen, "%s", a.name.c_str());
    return 0;
  }
  if (cmd == kASHMEM_SET_SIZE) {
    size_t sz = *(size_t*)arg;
    if (a.size != 0 && a.size != sz) return -EINVAL;
    a.size = sz;
#ifdef AOTDROID_APPLE
    if (a.shmHost >= 0 && ftruncate(a.shmHost, (off_t)sz) != 0) return -ENOMEM;
    if (a.map) munmap(a.map, a.size ? a.size : 1);
    a.map = a.shmHost >= 0
      ? mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, a.shmHost, 0)
      : nullptr;
    if (a.map == MAP_FAILED) { a.map = nullptr; return -ENOMEM; }
#endif
    return 0;
  }
  if (cmd == kASHMEM_GET_SIZE) return (long)a.size;
  if (cmd == kASHMEM_SET_PROT_MASK) return 0;
  if (cmd == kASHMEM_PIN || cmd == kASHMEM_UNPIN) {
    AshmemPin p; memcpy(&p, arg, sizeof(p));
    if (p.offset + (uint64_t)p.len > a.size) return -EINVAL;
    if (cmd == kASHMEM_PIN) {
      a.pinRanges.push_back({p.offset, p.len});
      a.pinned += p.len;
      return 0;      // ASHMEM_NOT_PINNED would be 1 if was unpinned
    }
    // unpin: madvise the range so iOS can reclaim under pressure
    bool wasPinned = false;
    for (size_t i = 0; i < a.pinRanges.size(); ) {
      auto& r = a.pinRanges[i];
      if (r.first == p.offset && r.second == p.len) {
        wasPinned = true;
        a.pinned -= p.len;
        std::swap(r, a.pinRanges.back());
        a.pinRanges.pop_back();
      } else ++i;
    }
#ifdef AOTDROID_APPLE
    if (a.map && p.len)
      madvise((uint8_t*)a.map + p.offset, p.len, MADV_FREE_REUSABLE);
#else
    (void)0;
#endif
    return wasPinned ? 0 : 1;
  }
  if (cmd == kASHMEM_PURGE_ALL_CACHES) { purgeAllUnpinned(); return 0; }
  return -ENOTTY;
}

void AshmemEmu::purgeAllUnpinned() {
  std::lock_guard<std::mutex> lk(mu_);
#ifdef AOTDROID_APPLE
  for (auto& kv : areas_) {
    Area& a = kv.second;
    if (!a.map) continue;
    size_t unpinned = a.size > a.pinned ? a.size - a.pinned : 0;
    if (unpinned == 0) continue;
    madvise(a.map, a.size, MADV_FREE_REUSE);
    madvise(a.map, a.size, MADV_FREE_REUSABLE);
  }
#endif
}

size_t BinderEmu::transactionsDelivered() const { return impl().txns.load(); }

}  // namespace aotdroid
