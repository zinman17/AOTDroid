// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/binder_emu.h   (MODULE 3)
//  In-process Linux Binder driver emulator supporting the legacy single-
//  domain /dev/binder (Android 7.1) and the Project Treble tri-domain
//  /dev/binder + /dev/hwbinder + /dev/vndbinder split (Android 8.1 / 10).
//
//  Kernel ABI mirrored here (uapi/linux/android/binder.h):
//    BINDER_WRITE_READ, BC_TRANSACTION/BC_REPLY, BR_TRANSACTION/BR_REPLY,
//    flat_binder_object handle translation, BC_INCREFS/ACQUIRE/RELEASE/DECREFS,
//    BC_FREE_BUFFER, BC_ENTER/EXIT/REGISTER_LOOPER, BR_SPAWN_LOOPER,
//    death notifications, BINDER_SET_CONTEXT_MGR(_EXT), BINDER_VERSION.
//  Per-release protocol versions: 7.1 -> 6, 8.1 -> 7, 10 -> 8.
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include "vfs.h"

namespace aotdroid {

// ---------------------------------------------------------------------------
// Wire structs (kernel layout, LP64).
// ---------------------------------------------------------------------------
#pragma pack(push, 8)
struct binder_write_read {
  uint64_t write_size, write_consumed;
  uint64_t write_buffer;        // BC_* command stream
  uint64_t read_size, read_consumed;
  uint64_t read_buffer;         // BR_* return stream
};
struct binder_transaction_data {
  union { uint32_t handle; uint64_t ptr; } target;
  uint64_t cookie;
  uint32_t code, flags;
  int32_t sender_pid; uint32_t sender_euid;
  uint64_t data_size, offsets_size;
  union {
    struct { uint64_t buffer; uint64_t offsets; } ptr;
    uint8_t buf[8];
  } data;
};
struct binder_ptr_cookie { uint64_t ptr; uint64_t cookie; };
struct binder_pri_desc { int32_t priority; uint32_t desc; };
struct binder_pri_ptr_cookie { int32_t priority; uint64_t ptr; uint64_t cookie; };
struct flat_binder_object {
  uint32_t type, flags;
  uint64_t binder;    /* local: ptr; remote: handle */
  uint64_t cookie;
};
struct binder_version { int32_t protocol_version; };
struct binder_node_debug_info { uint64_t ptr; uint64_t cookie; uint32_t has_strong_ref, has_weak_ref; };
#pragma pack(pop)

// object types (kernel B_PACK_CHARS encodings)
constexpr uint32_t kBINDER_TYPE_BINDER       = 0x73622a00;
constexpr uint32_t kBINDER_TYPE_WEAK_BINDER  = 0x77622a00;
constexpr uint32_t kBINDER_TYPE_HANDLE       = 0x73682a00;
constexpr uint32_t kBINDER_TYPE_WEAK_HANDLE  = 0x77682a00;
constexpr uint32_t kBINDER_TYPE_FD           = 0x66642a00;
constexpr uint32_t kBINDER_TYPE_FDA          = 0x66646100;
constexpr uint32_t kBINDER_TYPE_PTR          = 0x70742a00;

// transaction flags
constexpr uint32_t kTF_ONE_WAY = 0x01, kTF_ROOT_OBJECT = 0x04,
                   kTF_STATUS_CODE = 0x08, kTF_ACCEPT_FDS = 0x10;

// ioctls (values computed with the BSD _IOWR macros at run time; see .cpp)
extern const uint32_t kBINDER_WRITE_READ;
extern const uint32_t kBINDER_SET_MAX_THREADS;
extern const uint32_t kBINDER_SET_CONTEXT_MGR;
extern const uint32_t kBINDER_SET_CONTEXT_MGR_EXT;
extern const uint32_t kBINDER_THREAD_EXIT;
extern const uint32_t kBINDER_VERSION;
extern const uint32_t kBINDER_GET_NODE_DEBUG_INFO;
extern const uint32_t kBINDER_ENABLE_ONEWAY_SPAM_DETECTION;

// BC_* / BR_* (encoded at run time; mirrors of the kernel enum)
enum BinderCmd : uint32_t {
  BC_TRANSACTION_ = 1, BC_REPLY_, BC_ACQUIRE_RESULT, BC_FREE_BUFFER_,
  BC_INCREFS, BC_ACQUIRE, BC_RELEASE, BC_DECREFS,
  BC_INCREFS_DONE, BC_ACQUIRE_DONE, BC_ATTEMPT_ACQUIRE,
  BC_REQUEST_DEATH_NOTIFICATION, BC_CLEAR_DEATH_NOTIFICATION,
  BC_DEAD_BINDER_DONE, BC_TRANSACTION_SG, BC_REPLY_SG,
  BC_ENTER_LOOPER, BC_REGISTER_LOOPER, BC_EXIT_LOOPER,
  BR_ERROR_, BR_OK, BR_TRANSACTION_, BR_REPLY_, BR_DEAD_REPLY,
  BR_TRANSACTION_COMPLETE, BR_INCREFS, BR_ACQUIRE, BR_RELEASE,
  BR_DECREFS, BR_ATTEMPT_ACQUIRE, BR_NOOP, BR_SPAWN_LOOPER,
  BR_DEAD_BINDER, BR_CLEAR_DEATH_NOTIFICATION_DONE, BR_FAILED_REPLY,
  kBinderCmdCount
};
extern uint32_t g_bc[kBinderCmdCount];   // BC_* encodings (raw ioctl-word values)
extern uint32_t g_br[kBinderCmdCount];   // BR_* encodings

// ---------------------------------------------------------------------------
// Process domain tag: which "process" a thread belongs to. Domains are the
// single-process replacement for fork()ed Android processes (see domain.h).
// ---------------------------------------------------------------------------
using DomainId = uint32_t;
constexpr DomainId kInvalidDomain = 0;

struct BinderDomain;      // one of: binder / hwbinder / vndbinder contexts
struct BinderThreadState;

struct BinderTxnBuffer {
  std::vector<uint8_t> data;
  std::vector<uint64_t> offsets;     // offsets of flat_binder_objects
  DomainId owner;                    // domain that allocated it
  uint64_t id = 0;
};

struct BinderRef {
  uint32_t desc;                     // handle number (per domain-process)
  uint32_t node;
  int32_t strong = 0, weak = 0;
  bool dead = false;
};

struct BinderNode {
  uint64_t ptr = 0, cookie = 0;
  DomainId owner = kInvalidDomain;
  int32_t strong = 0, weak = 0;
  bool hasStrongRef = false, hasWeakRef = false;
  bool dead = false;
  std::string debugName;
};

// Per (domain-context, thread) state.
// Per-thread binder state is internal to binder_emu.cpp (ThrState).

// ---------------------------------------------------------------------------
class BinderEmu {
 public:
  static BinderEmu& get();

  void configure(uint32_t androidApi);          // protocol version per release
  uint32_t protocolVersion() const;

  // Creates the three context domains. Idempotent.
  void initContexts();
  BinderDomain* context(int which);             // 0/1/2

  // Entry from the syscall bridge: ioctl(fd, cmd, arg).
  long ioctl(int which, uint32_t cmd, void* arg, DomainId caller);

  // Node / handle plumbing used by the isolator when domains exit.
  void domainExit(DomainId d);

  // servicemanager support: node for the context manager.
  uint64_t contextManagerNode(int which) const;

  // Delivery of a transaction into a target thread's read queue.
  bool enqueueTransaction(int which, DomainId targetDomain, uint32_t targetHandle,
                          uint32_t code, uint32_t flags,
                          const uint8_t* data, size_t dataLen,
                          const uint64_t* offsets, size_t offsetsLen,
                          int32_t senderPid, uint32_t senderEuid,
                          uint64_t* outReplyId);

  size_t nodesAlive() const;
  size_t transactionsDelivered() const;

 private:
  BinderEmu() = default;
};

// Context-manager service number (servicemanager protocol)
constexpr uint32_t kSvcMgrGetService      = 1;
constexpr uint32_t kSvcMgrCheckService    = 2;
constexpr uint32_t kSvcMgrAddService      = 3;
constexpr uint32_t kSvcMgrListServices    = 4;

// ---------------------------------------------------------------------------
// /dev/ashmem emulation (module 3, part 2): shared memory backed by Darwin
// shm_open + mach_vm_allocate, with pin/unpin ranges and purge support.
// ---------------------------------------------------------------------------
constexpr uint32_t kAshmemNameLen = 256;

struct AshmemPin { uint32_t offset, len; };

class AshmemEmu {
 public:
  static AshmemEmu& get();
  long ioctl(VNode* vn, uint32_t cmd, void* arg);
  int create(const char* name, size_t size);   // returns emulated fd
  void purgeAllUnpinned();
  size_t pinnedBytes() const { return pinned_; }
 private:
  struct Area {
    std::string name;
    size_t size = 0, pinned = 0;
    int shmHost = -1;          // shm fd (Darwin) or memfd (dev)
    void* map = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> pinRanges;
  };
  std::mutex mu_;
  std::unordered_map<VNode*, Area> areas_;
  size_t pinned_ = 0;
  uint32_t nextId_ = 1;
};

// ashmem ioctls (from bionic's linux/ashmem.h, 'w' magic = 0x77)
extern const uint32_t kASHMEM_SET_NAME;
extern const uint32_t kASHMEM_GET_NAME;
extern const uint32_t kASHMEM_SET_SIZE;
extern const uint32_t kASHMEM_GET_SIZE;
extern const uint32_t kASHMEM_SET_PROT_MASK;
extern const uint32_t kASHMEM_PIN;
extern const uint32_t kASHMEM_UNPIN;
extern const uint32_t kASHMEM_GET_PIN_STATUS;
extern const uint32_t kASHMEM_PURGE_ALL_CACHES;

}  // namespace aotdroid
