// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/linker/syscall_bridge.cpp   (MODULE 4)
// ============================================================================
#include "syscall_bridge.h"
#include "../runtime/domain.h"
#include "../runtime/property_area.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#ifdef AOTDROID_APPLE
#include <os/proc.h>
#include <CommonCrypto/CommonRandom.h>
// NOTE: dispatch must be included at global scope, NEVER inside a namespace:
// dispatch/time.h forward-declares `struct timespec`, which would otherwise
// declare an incomplete aotdroid::timespec and hijack later lookups.
#include <dispatch/dispatch.h>
#endif

namespace aotdroid {

// Provided by gfx/input_hub.cpp and runtime/container_boot.cpp.
extern int inputHubOpen(int idx);
extern long inputHubIoctl(VNode* vn, uint32_t cmd, void* arg);
extern long propertyAreaRead(VNode* vn, void* buf, size_t n);

// ===========================================================================
// TLS emulation
// ===========================================================================
namespace {
pthread_key_t g_tlsKey = 0;
struct TlsKeyInit {
  TlsKeyInit() { pthread_key_create(&g_tlsKey, nullptr); }
} g_tlsKeyInit;
}  // namespace

extern "C" void* aotdroid_get_android_tls_c() {
  void* p = pthread_getspecific(g_tlsKey);
  if (!p) {
    p = calloc(1, sizeof(AndroidTlsBlock));
    ((AndroidTlsBlock*)p)->self = p;
    pthread_setspecific(g_tlsKey, p);
  }
  return p;
}
extern "C" void aotdroid_set_android_tls_c(void* tls) {
  if (tls) ((AndroidTlsBlock*)tls)->self = tls;
  pthread_setspecific(g_tlsKey, tls);
}
AndroidTlsBlock* currentAndroidTls() {
  return (AndroidTlsBlock*)aotdroid_get_android_tls_c();
}

// ===========================================================================
// arm64 asm entry (device builds): preserves x1..x17 + x30 across the bridge,
// mirroring the Linux kernel's syscall register contract, then tail-calls the
// C++ dispatcher. Host builds call the C symbol directly.
// ===========================================================================
#if defined(__aarch64__) && defined(AOTDROID_APPLE)
__asm__(
  ".globl _aotdroid_syscall_bridge\n"
  ".align 4\n"
  "_aotdroid_syscall_bridge:\n"
  "  stp x1, x2,  [sp, #-144]!\n"
  "  stp x3, x4,  [sp, #16]\n"
  "  stp x5, x6,  [sp, #32]\n"
  "  stp x7, x9,  [sp, #48]\n"    // x8 = syscall nr, kept live in place
  "  stp x10, x11,[sp, #64]\n"
  "  stp x12, x13,[sp, #80]\n"
  "  stp x14, x15,[sp, #96]\n"
  "  stp x16, x17,[sp, #112]\n"
  "  str x30,     [sp, #128]\n"
  "  mov x9, x0\n"                 // stash arg0
  "  mov x0, x8\n"                 // nr
  "  mov x1, x9\n"                 // a0
  "  ldr x2, [sp, #0]\n"           // a1
  "  ldr x3, [sp, #8]\n"           // a2
  "  ldr x4, [sp, #16]\n"          // a3
  "  ldr x5, [sp, #24]\n"          // a4
  "  ldr x6, [sp, #32]\n"          // a5
  "  bl  _aotdroid_syscall_c\n"
  "  ldp x1, x2,  [sp, #0]\n"
  "  ldp x3, x4,  [sp, #16]\n"
  "  ldp x5, x6,  [sp, #32]\n"
  "  ldp x7, x9,  [sp, #48]\n"
  "  ldp x10, x11,[sp, #64]\n"
  "  ldp x12, x13,[sp, #80]\n"
  "  ldp x14, x15,[sp, #96]\n"
  "  ldp x16, x17,[sp, #112]\n"
  "  ldr x30,     [sp, #128]\n"
  "  add sp, sp, #144\n"
  "  ret\n"
);
extern "C" long aotdroid_syscall_c(long nr, long a0, long a1, long a2,
                                 long a3, long a4, long a5);
[[maybe_unused]] static long dispatch(long nr, long a0, long a1, long a2, long a3, long a4,
                     long a5) {
  return aotdroid_syscall_c(nr, a0, a1, a2, a3, a4, a5);
}
#else
// No asm trampoline on this target: the public entry is an alias of the C
// dispatcher, defined after the dispatcher body below.
extern "C" long aotdroid_syscall_c(long nr, long a0, long a1, long a2,
                                 long a3, long a4, long a5);
[[maybe_unused]] static long dispatch(long nr, long a0, long a1, long a2, long a3, long a4,
                     long a5) {
  return aotdroid_syscall_c(nr, a0, a1, a2, a3, a4, a5);
}
#endif

// ===========================================================================
// futex
// ===========================================================================
long futexWait(uint32_t* uaddr, uint32_t expected, const LinuxTimespec* ts,
               uint32_t bitset) {
  if (*uaddr != expected) return -EAGAIN;
#if defined(AOTDROID_APPLE)
  uint32_t timeoutUs = 0;   // 0 = forever
  if (ts) {
    int64_t ns = (int64_t)ts->sec * 1000000000LL + ts->nsec;
    if (ns < 0) return -ETIMEDOUT;
    timeoutUs = (uint32_t)std::min<int64_t>(ns / 1000, 0xFFFFFF00ull);
  }
  int r = __ulock_wait(UL_COMPARE_AND_WAIT64 | 0 /*no flags*/, uaddr, expected,
                       timeoutUs);
  if (r == 0) return 0;
  if (errno == ETIMEDOUT) return -ETIMEDOUT;
  if (errno == EINTR) return -ERESTARTSYS;
  return 0;   // woken by value change
#else
  (void)bitset;
  return -ENOSYS;
#endif
}

long futexWake(uint32_t* uaddr, uint32_t count, uint32_t bitset) {
#if defined(AOTDROID_APPLE)
  uint32_t op = UL_COMPARE_AND_WAIT64;
  if (count > 1) op |= ULF_WAKE_ALL;
  int r = __ulock_wake(op, uaddr, 0);
  if (r == 0) return (long)std::min<uint32_t>(count, 1);
  if (errno == ENOENT) return 0;
  return 0;
#else
  (void)uaddr; (void)count; (void)bitset;
  return -ENOSYS;
#endif
}

// ===========================================================================
// epoll over kqueue (+ poll-function virtual fds)
// ===========================================================================
namespace {
struct [[maybe_unused]] EpollInterest {
  int fd;
  EpollEvent ev;
};
struct [[maybe_unused]] EpollState {
  int kq = -1;
  std::vector<EpollInterest> interests;
  int wakePipe[2] = {-1, -1};
  ~EpollState() {
    if (kq >= 0) ::close(kq);
    for (int p : wakePipe) if (p >= 0) ::close(p);
  }
};
[[maybe_unused]] std::unordered_map<int, std::shared_ptr<EpollState>>& epollStates() {
  static std::unordered_map<int, std::shared_ptr<EpollState>> m;
  return m;
}
[[maybe_unused]] std::mutex& epollMu() { static std::mutex m; return m; }

[[maybe_unused]] short epToKqueue(uint32_t events, uint32_t* fflags) {
  short f = 0;
  *fflags = 0;
  if (events & kEPOLLIN)  f |= EVFILT_READ;
  if (events & kEPOLLOUT) f |= EVFILT_WRITE;
  return f;
}
[[maybe_unused]] uint32_t kqueueToEp(short filter, uint16_t flags) {
  uint32_t e = 0;
  if (filter == EVFILT_READ) e |= kEPOLLIN;
  if (filter == EVFILT_WRITE) e |= kEPOLLOUT;
  if (flags & EV_EOF) e |= kEPOLLHUP;
  if (flags & EV_ERROR) e |= kEPOLLERR;
  return e;
}
}  // namespace

int epollCreate1(int flags) {
#if defined(AOTDROID_APPLE)
  auto st = std::make_shared<EpollState>();
  st->kq = kqueue();
  if (st->kq < 0) return -errno;
  if (::pipe(st->wakePipe) != 0) return -errno;
  ::fcntl(st->wakePipe[0], F_SETFL, O_NONBLOCK);
  auto vn = std::make_shared<VNode>();
  vn->kind = VNKind::Epoll;
  vn->hostFd = st->kq;
  vn->ctx = st.get();
  int fd = fds().alloc(vn);
  {
    std::lock_guard<std::mutex> lk(epollMu());
    epollStates()[fd] = st;
  }
  (void)flags;
  return fd;
#else
  (void)flags;
  errno = ENOSYS;
  return -1;
#endif
}

int epollCtl(int epfd, int op, int fd, const EpollEvent* ev) {
#if defined(AOTDROID_APPLE)
  std::shared_ptr<EpollState> st;
  {
    std::lock_guard<std::mutex> lk(epollMu());
    auto it = epollStates().find(epfd);
    if (it == epollStates().end()) return -ENOENT;
    st = it->second;
  }
  auto node = fds().get(fd);
  if (!node && op != kEPOLL_CTL_DEL) return -EBADF;
  if (node && node->kind == VNKind::HostFile && node->hostFd >= 0) {
    // real host fd -> register with kqueue directly
    uint32_t fflags = 0;
    short filt = epToKqueue(ev ? ev->events : 0, &fflags);
    struct kevent ke;
    EV_SET(&ke, (uintptr_t)node->hostFd, filt,
           op == kEPOLL_CTL_DEL ? EV_DELETE : (EV_ADD | (ev->events & kEPOLLET ? EV_CLEAR : 0)),
           fflags, 0, (void*)(uintptr_t)fd);
    if (kevent(st->kq, &ke, 1, nullptr, 0, nullptr) != 0) return -errno;
  }
  // virtual fd: remember the interest; readiness is polled by pollFn
  {
    std::lock_guard<std::mutex> lk(epollMu());
    for (size_t i = 0; i < st->interests.size(); ++i) {
      if (st->interests[i].fd == fd) {
        if (op == kEPOLL_CTL_DEL) {
          st->interests.erase(st->interests.begin() + i);
          return 0;
        }
        st->interests[i].ev = *ev;
        return 0;
      }
    }
    if (op == kEPOLL_CTL_ADD) st->interests.push_back({fd, *ev});
  }
  return 0;
#else
  (void)epfd; (void)op; (void)fd; (void)ev;
  return -ENOSYS;
#endif
}

int epollWait(int epfd, EpollEvent* out, int maxEvents, int timeoutMs) {
#if defined(AOTDROID_APPLE)
  std::shared_ptr<EpollState> st;
  {
    std::lock_guard<std::mutex> lk(epollMu());
    auto it = epollStates().find(epfd);
    if (it == epollStates().end()) return -ENOENT;
    st = it->second;
  }
  int n = 0;
  auto pollVirtuals = [&]() -> int {
    int c = 0;
    std::lock_guard<std::mutex> lk(epollMu());
    for (const auto& in : st->interests) {
      auto node = fds().get(in.fd);
      if (!node || !node->pollFn) continue;
      uint32_t ready = node->pollFn(node.get());
      if (ready & in.ev.events) {
        if (n < maxEvents) { out[n].events = ready & in.ev.events; out[n].data = in.ev.data; }
        n++; c++;
      }
    }
    return c;
  };
  n = pollVirtuals();
  if (n > 0) return std::min(n, maxEvents);
  struct kevent kes[32];
  struct timespec ts{0, (int64_t)timeoutMs * 1000000};
  struct timespec* pts = timeoutMs < 0 ? nullptr : &ts;
  // also watch the wake pipe
  {
    struct kevent ke;
    EV_SET(&ke, (uintptr_t)st->wakePipe[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    kevent(st->kq, &ke, 1, nullptr, 0, nullptr);
  }
  int r = kevent(st->kq, nullptr, 0, kes, 32, pts);
  if (r < 0) return -errno;
  int emitted = 0;
  for (int i = 0; i < r && emitted < maxEvents; ++i) {
    if (kes[i].filter == EVFILT_READ && (int)(uintptr_t)kes[i].ident == st->wakePipe[0]) {
      char drain[64];
      while (::read(st->wakePipe[0], drain, sizeof(drain)) > 0) {}
      continue;
    }
    out[emitted].events = kqueueToEp(kes[i].filter, kes[i].flags);
    out[emitted].data = (uint64_t)(uintptr_t)kes[i].udata;
    emitted++;
  }
  if (emitted == 0) {
    n = pollVirtuals();
    return std::min(n, maxEvents);
  }
  return emitted;
#else
  (void)epfd; (void)out; (void)maxEvents; (void)timeoutMs;
  return -ENOSYS;
#endif
}

// ===========================================================================
// eventfd2 / timerfd
// ===========================================================================
namespace {
struct EventfdState {
  std::atomic<uint64_t> count{0};
  int semaphore;       // EFD_SEMAPHORE
  int wake[2] = {-1, -1};
};
struct TimerfdState {
  int wake[2] = {-1, -1};
  uint64_t expirations = 0;
  void* dispatchSource = nullptr;   // dispatch_source_t on Apple
  void* queue = nullptr;
  LinuxTimespec interval{0, 0};
};
[[maybe_unused]] uint32_t eventfdPoll(VNode* n) {
  auto* st = (EventfdState*)n->ctx;
  return st && st->count.load() > 0 ? kEPOLLIN : 0;
}
[[maybe_unused]] uint32_t timerfdPoll(VNode* n) {
  auto* st = (TimerfdState*)n->ctx;
  return st && st->expirations > 0 ? kEPOLLIN : 0;
}
}  // namespace

int eventfdCreate(unsigned initval, int flags) {
#if defined(AOTDROID_APPLE)
  auto st = std::make_unique<EventfdState>();
  st->count = initval;
  st->semaphore = flags & kEFD_SEMAPHORE;
  if (::pipe(st->wake) != 0) return -errno;
  ::fcntl(st->wake[0], F_SETFL, O_NONBLOCK);
  ::fcntl(st->wake[1], F_SETFL, O_NONBLOCK);
  auto vn = std::make_shared<VNode>();
  vn->kind = VNKind::Eventfd;
  vn->ctx = st.release();
  vn->pollFn = eventfdPoll;
  return fds().alloc(vn);
#else
  (void)initval; (void)flags;
  return -ENOSYS;
#endif
}

int timerfdCreate(int clockid, int flags) {
#if defined(AOTDROID_APPLE)
  (void)clockid; (void)flags;
  auto st = std::make_unique<TimerfdState>();
  if (::pipe(st->wake) != 0) return -errno;
  ::fcntl(st->wake[0], F_SETFL, O_NONBLOCK);
  ::fcntl(st->wake[1], F_SETFL, O_NONBLOCK);
  auto vn = std::make_shared<VNode>();
  vn->kind = VNKind::Timerfd;
  vn->ctx = st.release();
  vn->pollFn = timerfdPoll;
  return fds().alloc(vn);
#else
  (void)clockid; (void)flags;
  return -ENOSYS;
#endif
}

int timerfdSettime(int fd, int flags, const LinuxTimespec* itValue,
                   const LinuxTimespec* itInterval, LinuxTimespec* oldValue) {
#if defined(AOTDROID_APPLE)
  auto vn = fds().get(fd);
  if (!vn || vn->kind != VNKind::Timerfd) return -EINVAL;
  auto* st = (TimerfdState*)vn->ctx;
  if (oldValue) *oldValue = st->interval;
  st->interval = itInterval ? *itInterval : LinuxTimespec{0, 0};
  if (st->dispatchSource) {
    dispatch_source_cancel((dispatch_source_t)st->dispatchSource);
    st->dispatchSource = nullptr;
  }
  if (!itValue || (itValue->sec == 0 && itValue->nsec == 0)) return 0;
  uint64_t ns = (uint64_t)itValue->sec * 1000000000ull + (uint64_t)itValue->nsec;
  dispatch_queue_t q = (dispatch_queue_t)st->queue;
  if (!q) { q = dispatch_queue_create("aotdroid.timerfd", DISPATCH_QUEUE_SERIAL); st->queue = q; }
  dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
  dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ns),
                            (uint64_t)st->interval.sec * 1000000000ull + st->interval.nsec,
                            10000000ull);
  dispatch_source_set_event_handler(src, ^{
    st->expirations++;
    char c = 1;
    (void)!::write(st->wake[1], &c, 1);
    VNode::armWakeup(nullptr);
  });
  dispatch_resume(src);
  st->dispatchSource = src;
  return 0;
#else
  (void)fd; (void)flags; (void)itValue; (void)itInterval; (void)oldValue;
  return -ENOSYS;
#endif
}

// ===========================================================================
// BRK trampoline table (fallback for out-of-range ELF veneers)
// ===========================================================================
namespace {
struct BrkTable {
  std::mutex mu;
  std::vector<uint64_t> sites;       // new-VA of each patched site
  const uint8_t* patchSeg = nullptr;
  size_t patchLen = 0;
};
BrkTable& brk() { static BrkTable b; return b; }
}  // namespace

void brkTrapRegister(size_t count, const void* patchSeg, size_t segLen) {
  auto& b = brk();
  std::lock_guard<std::mutex> lk(b.mu);
  b.patchSeg = (const uint8_t*)patchSeg;
  b.patchLen = segLen;
  b.sites.resize(count);
}
long brkTrapDispatch(uint32_t idx, void* pc, uint64_t* regs) {
  auto& b = brk();
  std::lock_guard<std::mutex> lk(b.mu);
  if (idx >= b.sites.size()) return -EINVAL;
  // The real dispatch runs the syscall and resumes at pc+4; the Mach
  // exception server (container_boot) performs register fixup using this.
  (void)regs;
  return 0;
}

// ===========================================================================
// exec image registry
// ===========================================================================
namespace {
std::mutex& execMu() { static std::mutex m; return m; }
std::unordered_map<std::string, ExecImage>& execImages() {
  static std::unordered_map<std::string, ExecImage> m;
  return m;
}
}  // namespace
void execRegisterImage(const ExecImage& img) {
  std::lock_guard<std::mutex> lk(execMu());
  execImages()[img.path] = img;
}

// ===========================================================================
// Signal chain
// ===========================================================================
namespace {
SigChainEntry g_chain[64];
void masterHandler(int sig, siginfo_t* si, void* uc) {
  SigChainEntry& e = g_chain[sig];
  if (e.handler) e.handler(sig, si, uc);
}
}
SigChainEntry* sigChainGet(int sig) {
  if (sig <= 0 || sig >= 64) return nullptr;
  return &g_chain[sig];
}
void sigChainInstall() {
  struct sigaction sa{};
  sa.sa_sigaction = masterHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGPIPE, SIGTRAP}) {
    sigaction(s, &sa, nullptr);
  }
}

// ===========================================================================
// VFS open + helpers
// ===========================================================================
namespace {
int hostOpenFlags(long linuxFlags) {
  int f = (int)(linuxFlags & (kO_RDONLY | kO_WRONLY | kO_RDWR | kO_CREAT |
                              kO_EXCL | kO_TRUNC | kO_APPEND | kO_NONBLOCK));
#ifdef O_CLOEXEC
  if (linuxFlags & kO_CLOEXEC) f |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
  if (linuxFlags & kO_DIRECTORY) f |= O_DIRECTORY;
#endif
  return f;
}

int openSpecial(PathTranslator::Special sp, long flags, const std::string& path) {
  auto vn = std::make_shared<VNode>();
  vn->openFlags = (uint32_t)flags;
  switch (sp) {
    case PathTranslator::Special::Binder:
    case PathTranslator::Special::HwBinder:
    case PathTranslator::Special::VndBinder: {
      vn->kind = VNKind::Binder;
      vn->cookie = sp == PathTranslator::Special::Binder ? 0
                 : (sp == PathTranslator::Special::HwBinder ? 1 : 2);
      return fds().alloc(vn);
    }
    case PathTranslator::Special::Ashmem:
    case PathTranslator::Special::AshmemLegacy: {
      vn->kind = VNKind::Ashmem;
      vn->name = "ashmem";
      return fds().alloc(vn);
    }
    case PathTranslator::Special::InputEvent0:
    case PathTranslator::Special::InputEvent1: {
      return inputHubOpen(sp == PathTranslator::Special::InputEvent0 ? 0 : 1);
    }
    case PathTranslator::Special::Null: {
      int h = ::open("/dev/null", O_RDWR);
      if (h < 0) return -ENOENT;
      vn->kind = VNKind::NullDev; vn->hostFd = h;
      return fds().alloc(vn);
    }
    case PathTranslator::Special::Zero: {
      int h = ::open("/dev/zero", O_RDONLY);
      if (h < 0) return -ENOENT;
      vn->kind = VNKind::NullDev; vn->hostFd = h;
      return fds().alloc(vn);
    }
    case PathTranslator::Special::Urandom:
    case PathTranslator::Special::Random: {
      int h = ::open("/dev/urandom", O_RDONLY);
      if (h < 0) return -ENOENT;
      vn->kind = VNKind::RandomDev; vn->hostFd = h;
      return fds().alloc(vn);
    }
    case PathTranslator::Special::ProcSelfMaps:
    case PathTranslator::Special::ProcSelfCmdline:
    case PathTranslator::Special::ProcCpuinfo:
    case PathTranslator::Special::SysCpuPresent:
    case PathTranslator::Special::SysCpuMaxFreq: {
      vn->kind = VNKind::Synth;
      vn->name = path;
      if (sp == PathTranslator::Special::ProcSelfMaps) vn->ctx = (void*)1;
      if (sp == PathTranslator::Special::ProcSelfCmdline) vn->ctx = (void*)2;
      if (sp == PathTranslator::Special::ProcCpuinfo) vn->ctx = (void*)3;
      if (sp == PathTranslator::Special::SysCpuPresent) vn->ctx = (void*)4;
      if (sp == PathTranslator::Special::SysCpuMaxFreq) vn->ctx = (void*)5;
      return fds().alloc(vn);
    }
    case PathTranslator::Special::DevProperties: {
      vn->kind = VNKind::PropArea;
      vn->name = path;
      return fds().alloc(vn);
    }
    default:
      return -ENOENT;
  }
}
std::string synthContent(const VNode* n) {
  switch ((uintptr_t)n->ctx) {
    case 1: return genProcSelfMaps();
    case 2: return genProcSelfCmdline();
    case 3: return genProcCpuinfo();
    case 4: return genSysCpuPresent();
    case 5: return genSysCpuMaxFreq();
  }
  return "";
}
}  // namespace

// ===========================================================================
// The dispatcher
// ===========================================================================
extern "C" long aotdroid_syscall_c(long nr, long a0, long a1, long a2,
                                 long a3, long a4, long a5) {
  switch (nr) {
    // ---- identity / proc -------------------------------------------------
    case sys::getpid:  return (long)domains().currentPid();
    case sys::getppid: return (long)domains().parentPid();
    case sys::gettid:  return (long)domains().currentTid();
    case sys::getuid: case sys::geteuid: return 0;
    case sys::getgid: case sys::getegid: return 0;
    case sys::setuid: case sys::setgid: return 0;
    case sys::personality: return 0;
    case sys::capget: case sys::capset: return 0;

    // ---- file I/O --------------------------------------------------------
    case sys::openat: {
      const char* path = (const char*)(uintptr_t)a1;
      if (!path) return -EFAULT;
      std::string p(path);
      auto sp = PathTranslator::classify(p);
      if (sp != PathTranslator::Special::None) {
        // writable property area opens are allowed; read-only for the rest
        int fd = openSpecial(sp, a2, p);
        if (fd < 0 && fd != -ENOENT) return (long)fd;
        if (fd >= 0) return fd;
        return -ENOENT;
      }
      // pack first (read-only rootfs), then real container paths
      if (rootfsPack().lookup(p, nullptr, nullptr)) {
        auto vn = std::make_shared<VNode>();
        vn->kind = VNKind::HostFile;
        vn->name = p;
        vn->cookie = 1;   // pack-backed
        return fds().alloc(vn);
      }
      std::string host = paths().translate(p);
      int h = ::open(host.c_str(), hostOpenFlags(a2), (mode_t)a3);
      if (h < 0) return -errno;
      auto vn = std::make_shared<VNode>();
      vn->kind = VNKind::HostFile;
      vn->hostFd = h;
      vn->name = p;
      return fds().alloc(vn);
    }
    case sys::close: {
      auto vn = fds().release((int)a0);
      if (!vn) return -EBADF;
      if (vn->kind == VNKind::HostFile && vn->hostFd >= 0 && !vn->cookie)
        ::close(vn->hostFd);
      return 0;
    }
    case sys::read: {
      auto vn = fds().get((int)a0);
      if (!vn) return -EBADF;
      if (vn->kind == VNKind::Synth) {
        std::string s = synthContent(vn.get());
        if (vn->pos >= s.size()) return 0;
        size_t n = std::min<size_t>((size_t)a2, s.size() - (size_t)vn->pos);
        memcpy((void*)(uintptr_t)a1, s.data() + vn->pos, n);
        vn->pos += n;
        return (long)n;
      }
      if (vn->kind == VNKind::HostFile && vn->cookie == 1) {
        // pack-backed read
        PackEntry pe; const uint8_t* data = nullptr;
        if (!rootfsPack().lookup(vn->name, &pe, &data)) return -ENOENT;
        if (vn->pos >= pe.size) return 0;
        size_t n = std::min<size_t>((size_t)a2, (size_t)(pe.size - vn->pos));
        memcpy((void*)(uintptr_t)a1, data + vn->pos, n);
        vn->pos += n;
        return (long)n;
      }
      if (vn->kind == VNKind::PropArea) {
          return propertyAreaRead(vn.get(), (void*)(uintptr_t)a1, (size_t)a2);
      }
      if (vn->kind == VNKind::Eventfd) {
        auto* st = (EventfdState*)vn->ctx;
        if (a2 < 8) return -EINVAL;
        uint64_t v = st->count.exchange(0);
        if (st->semaphore && v) st->count = v - 1;
        memcpy((void*)(uintptr_t)a1, &v, 8);
        return 8;
      }
      if (vn->kind == VNKind::Timerfd) {
        auto* st = (TimerfdState*)vn->ctx;
        if (a2 < 8) return -EINVAL;
        uint64_t v = st->expirations; st->expirations = 0;
        memcpy((void*)(uintptr_t)a1, &v, 8);
        return 8;
      }
      if (vn->hostFd >= 0) {
        ssize_t r = ::read(vn->hostFd, (void*)(uintptr_t)a1, (size_t)a2);
        return r < 0 ? -errno : (long)r;
      }
      return -EBADF;
    }
    case sys::write: {
      auto vn = fds().get((int)a0);
      if (!vn) return -EBADF;
      if (vn->kind == VNKind::Eventfd) {
        auto* st = (EventfdState*)vn->ctx;
        uint64_t v;
        if (a2 < 8) return -EINVAL;
        memcpy(&v, (const void*)(uintptr_t)a1, 8);
        st->count += v;
        char c = 1;
        (void)!::write(st->wake[1], &c, 1);
        VNode::armWakeup(nullptr);
        return 8;
      }
      if (vn->hostFd >= 0) {
        ssize_t r = ::write(vn->hostFd, (const void*)(uintptr_t)a1, (size_t)a2);
        return r < 0 ? -errno : (long)r;
      }
      return -EBADF;
    }
    case sys::writev: case sys::readv: {
      auto vn = fds().get((int)a0);
      if (!vn) return -EBADF;
      struct iovec* iov = (struct iovec*)(uintptr_t)a1;
      int cnt = (int)a2;
      long total = 0;
      for (int i = 0; i < cnt; ++i) {
        long r = aotdroid_syscall_c(nr == sys::writev ? sys::write : sys::read,
                                    a0, (long)(uintptr_t)iov[i].iov_base,
                                    (long)iov[i].iov_len, 0, 0, 0);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
      }
      return total;
    }
    case sys::lseek: {
      auto vn = fds().get((int)a0);
      if (!vn) return -EBADF;
      if (vn->kind == VNKind::Synth || vn->cookie == 1) {
        vn->pos = (uint64_t)a1;
        return (long)vn->pos;
      }
      off_t r = ::lseek(vn->hostFd, (off_t)a1, (int)a2);
      return r < 0 ? -errno : (long)r;
    }
    case sys::fstat:
    case 79 /*newfstatat*/: {
      // minimal stat: satisfies ART's dex-file checks
      auto vn = nr == sys::fstat ? fds().get((int)a0) : nullptr;
      LinuxStat64* st = (LinuxStat64*)(uintptr_t)(nr == sys::fstat ? a1 : a2);
      if (!st) return -EFAULT;
      memset(st, 0, sizeof(*st));
      st->st_mode = 0100755;
      st->st_blksize = 4096;
      st->st_nlink = 1;
      if (vn && vn->cookie == 1) {
        PackEntry pe;
        if (rootfsPack().lookup(vn->name, &pe, nullptr)) {
          st->st_size = (int64_t)pe.size;
          uint32_t fmt = 0100000;
          if (pe.flags & kPackFlagDir) fmt = 0040000;
          else if (pe.flags & kPackFlagSymlink) fmt = 0120000;
          else if (pe.mode & 0111) fmt = 0100755;
          st->st_mode = fmt | (pe.mode & 07777);
          st->st_uid = pe.uid;
          st->st_gid = pe.gid;
          st->st_ino = 0x5000 + pe.blobOff / 4096;
        }
      } else if (vn && vn->kind == VNKind::HostFile) {
        struct ::stat hs;
        if (::fstat(vn->hostFd, &hs) == 0) {
          st->st_size = hs.st_size;
          st->st_mode = (uint32_t)hs.st_mode;
          st->st_ino = (uint64_t)hs.st_ino;
          st->st_dev = (uint64_t)hs.st_dev;
        }
      }
      return 0;
    }
    case sys::getdents64: {
      auto vn = fds().get((int)a0);
      if (!vn) return -EBADF;
      if (vn->cookie == 1) {   // pack directory
        auto entries = rootfsPack().listDir(vn->name);
        auto entryType = [&](const std::string& nm) -> uint8_t {
          std::string full = vn->name + "/" + nm;
          PackEntry pe;
          if (rootfsPack().lookupRaw(full, &pe, nullptr)) {
            if (pe.flags & kPackFlagDir) return 4;      // DT_DIR
            if (pe.flags & kPackFlagSymlink) return 10;  // DT_LNK
            return 8;                                    // DT_REG
          }
          return 4;   // legacy packs without dir entries: subdir name
        };
        uint8_t* out = (uint8_t*)(uintptr_t)a1;
        size_t cap = (size_t)a2, used = 0, i = (size_t)vn->pos;
        while (i < entries.size()) {
          const std::string& nm = entries[i];
          uint16_t reclen = (uint16_t)(offsetof(LinuxDirent64, d_name) + nm.size() + 1);
          reclen = (uint16_t)((reclen + 7) & ~7);
          if (used + reclen > cap) break;
          LinuxDirent64 d{};
          d.d_ino = 1000 + i;
          d.d_off = (int64_t)(i + 1);
          d.d_reclen = reclen;
          d.d_type = (nm == "." || nm == "..") ? 4 : entryType(nm);
          memcpy(d.d_name, nm.c_str(), nm.size() + 1);
          memcpy(out + used, &d, reclen);
          used += reclen;
          i++;
        }
        vn->pos = i;
        return (long)used;
      }
      if (vn->kind == VNKind::HostDir && vn->hostFd >= 0) {
        uint8_t* out = (uint8_t*)(uintptr_t)a1;
        size_t cap = (size_t)a2, used = 0;
        while (used + 280 <= cap) {
          struct dirent* de = ::readdir((DIR*)(uintptr_t)vn->ctx);
          if (!de) break;
          if (strncmp(de->d_name, "._", 2) == 0 || strcmp(de->d_name, ".DS_Store") == 0)
            continue;   // hide AppleDouble junk from Android
          uint16_t reclen = (uint16_t)((offsetof(LinuxDirent64, d_name)
                                        + strlen(de->d_name) + 1 + 7) & ~7);
          LinuxDirent64 d{};
          d.d_ino = de->d_ino;
          d.d_off = d.d_ino;
          d.d_reclen = reclen;
          d.d_type = de->d_type;
          memcpy(d.d_name, de->d_name, strlen(de->d_name) + 1);
          memcpy(out + used, &d, reclen);
          used += reclen;
        }
        return (long)used;
      }
      return -ENOTDIR;
    }
    case sys::faccessat: {
      const char* path = (const char*)(uintptr_t)a1;
      if (!path) return -EFAULT;
      std::string p(path);
      if (rootfsPack().lookup(p, nullptr, nullptr)) return 0;
      if (::access(paths().translate(p).c_str(), (int)a2) == 0) return 0;
      return -ENOENT;
    }
    case sys::readlinkat: {
      const char* path = (const char*)(uintptr_t)a1;
      char* buf = (char*)(uintptr_t)a2;
      size_t bufsiz = (size_t)a3;
      if (!path || !buf) return -EFAULT;
      std::string p(path);
      PackEntry pe;
      const uint8_t* data = nullptr;
      if (!rootfsPack().lookupRaw(p, &pe, &data)) {
        // host-side symlink? fall back to the container path
        char tmp[4096];
        ssize_t r = ::readlink(paths().translate(p).c_str(), tmp, sizeof(tmp) - 1);
        if (r <= 0) return -ENOENT;
        size_t n = std::min<size_t>((size_t)r, bufsiz);
        memcpy(buf, tmp, n);
        return (long)n;
      }
      if (!(pe.flags & kPackFlagSymlink)) return -EINVAL;
      size_t n = std::min<size_t>((size_t)pe.size, bufsiz);
      memcpy(buf, data, n);
      return (long)n;
    }
    case sys::mkdirat: case sys::unlinkat: case sys::renameat: case sys::fchmodat:
      return -EROFS;
    case sys::getcwd: {
      std::string cwd = domains().currentCwd();
      if ((size_t)a1 < cwd.size() + 1) return -ERANGE;
      snprintf((char*)(uintptr_t)a0, (size_t)a1, "%s", cwd.c_str());
      return (long)cwd.size();
    }
    case sys::chdir: domains().setCwd((const char*)(uintptr_t)a0); return 0;

    case sys::fcntl: {
      auto vn = fds().get((int)a0);
      if (!vn) return -EBADF;
      if (a1 == kF_GETFD) return (vn->openFlags & kO_CLOEXEC) ? kFD_CLOEXEC : 0;
      if (a1 == kF_SETFD) {
        if (a2 & kFD_CLOEXEC) vn->openFlags |= (uint32_t)kO_CLOEXEC;
        else vn->openFlags &= ~((uint32_t)kO_CLOEXEC);
        return 0;
      }
      if (a1 == kF_GETFL) return (long)vn->openFlags;
      if (a1 == kF_SETFL) { vn->openFlags = (uint32_t)a2; return 0; }
      return -EINVAL;
    }
    case sys::dup: {
      int r = fds().dup((int)a0);
      return r < 0 ? -EBADF : r;
    }
    case sys::dup3: {
      int r = fds().dup((int)a0, (int)a1);
      return r < 0 ? -EBADF : r;
    }
    case sys::pipe2: return -ENOSYS;

    // ---- ioctls ----------------------------------------------------------
    case sys::ioctl: {
      auto vn = fds().get((int)a0);
      if (!vn) return -EBADF;
      if (vn->kind == VNKind::Binder)
        return BinderEmu::get().ioctl((int)vn->cookie, (uint32_t)a1, (void*)(uintptr_t)a2,
                                      domains().current());
      if (vn->kind == VNKind::Ashmem)
        return AshmemEmu::get().ioctl(vn.get(), (uint32_t)a1, (void*)(uintptr_t)a2);
      if (vn->kind == VNKind::Evdev)
        return inputHubIoctl(vn.get(), (uint32_t)a1, (void*)(uintptr_t)a2);
      if (vn->hostFd >= 0) {
        int r = ::ioctl(vn->hostFd, (unsigned long)a1, (void*)(uintptr_t)a2);
        return r < 0 ? -errno : r;
      }
      return -ENOTTY;
    }

    // ---- synchronization ---------------------------------------------------
    case sys::futex: {
      uint32_t* uaddr = (uint32_t*)(uintptr_t)a0;
      int op = (int)(a1 & 0x7f);
      switch (op) {
        case kFUTEX_WAIT:
        case kFUTEX_WAIT_BITSET: {
          LinuxTimespec ts{};
          if (a3) memcpy(&ts, (const void*)(uintptr_t)a3, sizeof(ts));
          return futexWait(uaddr, (uint32_t)a2, a3 ? &ts : nullptr,
                           (uint32_t)a4);
        }
        case kFUTEX_WAKE:
        case kFUTEX_WAKE_BITSET:
          return futexWake(uaddr, (uint32_t)a2, (uint32_t)a4);
        default:
          return -ENOSYS;
      }
    }
    case sys::set_robust_list: return 0;
    case sys::set_tid_address: return (long)domains().currentTid();

    // ---- epoll / eventfd / timerfd ---------------------------------------
    case sys::epoll_create1: {
      return (long)epollCreate1((int)a0);
    }
    case sys::epoll_ctl: {
      return (long)epollCtl((int)a0, (int)a1, (int)a2,
                            (const EpollEvent*)(uintptr_t)a3);
    }
    case sys::epoll_pwait: {
      return (long)epollWait((int)a0, (EpollEvent*)(uintptr_t)a1, (int)a2,
                             a3 ? (int)(*(int64_t*)(uintptr_t)a3) / 1000000 : -1);
    }
    case sys::eventfd2: {
      int r = eventfdCreate((unsigned)a0, (int)a1);
      return r < 0 ? -errno : r;
    }
    case sys::timerfd_create: {
      int r = timerfdCreate((int)a0, (int)a1);
      return r < 0 ? -errno : r;
    }
    case sys::timerfd_settime: {
      // a2 = flags; a3 = struct itimerspec*
      const LinuxTimespec* val = a3 ? (const LinuxTimespec*)(uintptr_t)a3 : nullptr;
      const LinuxTimespec* iv = val ? val + 1 : nullptr;
      return timerfdSettime((int)a0, (int)a2, val, iv, (LinuxTimespec*)(uintptr_t)a4);
    }
    case sys::timerfd_gettime: return -ENOSYS;

    // ---- memory ------------------------------------------------------------
    case sys::mmap: {
      void* p = mmap((void*)(uintptr_t)a0, (size_t)a1, (int)a2, (int)a3,
                     -1, 0);
      if (p == MAP_FAILED) return -errno;
      mmapRegistry().add((uint64_t)(uintptr_t)p, (size_t)a1, (uint32_t)a2,
                         (uint32_t)a3, 0, "[anon]", false);
      return (long)(uintptr_t)p;
    }
    case sys::munmap: {
      int r = munmap((void*)(uintptr_t)a0, (size_t)a1);
      mmapRegistry().remove((uint64_t)a0, (uint64_t)a1);
      return r == 0 ? 0 : -errno;
    }
    case sys::mprotect: {
      int r = mprotect((void*)(uintptr_t)a0, (size_t)a1, (int)a2);
      return r == 0 ? 0 : -errno;
    }
    case sys::madvise: {
#ifdef MADV_DONTNEED
      int r = madvise((void*)(uintptr_t)a0, (size_t)a1, (int)a2);
      return r == 0 ? 0 : -errno;
#else
      return 0;
#endif
    }
    case sys::mremap: {
      // Darwin has no mremap: allocate, copy, unmap. Moving a region with
      // live futexes is not supported (documented limitation; ART only
      // mremaps its heap for growth, which our allocator satisfies).
      void* oldp = (void*)(uintptr_t)a0;
      size_t oldLen = (size_t)a1, newLen = (size_t)a2;
      void* np = mmap(nullptr, newLen, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (np == MAP_FAILED) return -ENOMEM;
      memcpy(np, oldp, std::min(oldLen, newLen));
      munmap(oldp, oldLen);
      mmapRegistry().remove((uint64_t)a0, oldLen);
      mmapRegistry().add((uint64_t)(uintptr_t)np, newLen, PROT_READ | PROT_WRITE, 0,
                         0, "[anon:heap]", false);
      return (long)(uintptr_t)np;
    }
    case sys::msync: return 0;
    case sys::memfd_create: {
      auto vn = std::make_shared<VNode>();
      vn->kind = VNKind::Ashmem;   // memfd = anonymous shm here
      vn->name = "memfd";
      return fds().alloc(vn);
    }

    // ---- processes (fork-less isolator) ------------------------------------
    case sys::clone: {
      uint64_t flags = (uint64_t)a0;
      return domains().clone(flags, (void*)(uintptr_t)a1, (void*)(uintptr_t)a2,
                             (void*)(uintptr_t)a3, (void*)(uintptr_t)a4);
    }
    case sys::execve: {
      const char* path = (const char*)(uintptr_t)a0;
      std::lock_guard<std::mutex> lk(execMu());
      auto it = execImages().find(path ? path : "");
      if (it == execImages().end()) {
        AOTD_LOGE("syscall", "execve(%s): image not in registry", path ? path : "?");
        return -ENOENT;
      }
      domains().execve((const void*)&it->second, (char* const*)(uintptr_t)a1,
                       (char* const*)(uintptr_t)a2);
      return 0;   // never returns on success
    }
    case sys::exit: domains().threadExit((int)a0); return 0;
    case sys::exit_group: domains().domainExitCurrent((int)a0); return 0;
    case sys::wait4: return domains().wait4((int)a0, (int*)(uintptr_t)a1, (int)a2);
    case sys::tgkill: {
      // signal delivery inside the single process
      return domains().tgkill((int)a0, (int)a1, (int)a2);
    }

    // ---- time --------------------------------------------------------------
    case sys::clock_gettime: {
      auto* ts = (LinuxTimespec*)(uintptr_t)a1;
      struct timespec h{};
      clock_gettime(a0 == kCLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &h);
      if (ts) { ts->sec = h.tv_sec; ts->nsec = h.tv_nsec; }
      return 0;
    }
    case sys::clock_getres: {
      auto* ts = (LinuxTimespec*)(uintptr_t)a1;
      if (ts) { ts->sec = 0; ts->nsec = 1; }
      return 0;
    }
    case sys::nanosleep: {
      const LinuxTimespec* req = (const LinuxTimespec*)(uintptr_t)a0;
      struct timespec r{ (time_t)req->sec, (long)req->nsec };
      int rc = nanosleep(&r, nullptr);
      return rc == 0 ? 0 : -errno;
    }

    // ---- misc ---------------------------------------------------------------
    case sys::getrandom: {
      void* buf = (void*)(uintptr_t)a0;
      size_t n = (size_t)a1;
#ifdef AOTDROID_APPLE
      if (CCRandomGenerateBytes(buf, n) != kCCSuccess) return -EIO;
#else
      if (getentropy(buf, n) != 0) return -EIO;
#endif
      return (long)n;
    }
    case sys::prctl: {
      switch (a0) {
        case kPR_SET_NAME: {
          char nm[17] = {0};
          memcpy(nm, (const void*)(uintptr_t)a1, 16);
#ifdef __APPLE__
          pthread_setname_np(nm);
#else
          pthread_setname_np(pthread_self(), nm);
#endif
          domains().setThreadName(nm);
          return 0;
        }
        case kPR_GET_NAME: {
          const std::string& nm = domains().threadName();
          snprintf((char*)(uintptr_t)a1, 16, "%s", nm.c_str());
          return 0;
        }
        case kPR_SET_VMA:
          // PR_SET_VMA_ANON_NAME: bookkeeping only
          return 0;
        case kPR_SET_DUMPABLE: case kPR_GET_DUMPABLE: case kPR_SET_NO_NEW_PRIVS:
          return 0;
        default: return 0;
      }
    }
    case sys::rt_sigaction: {
      int sig = (int)a0;
      if (sig <= 0 || sig >= 64) return -EINVAL;
      SigChainEntry* e = sigChainGet(sig);
      if (a2) {   // old
        memcpy((void*)(uintptr_t)a2, e, sizeof(*e));
      }
      if (a1) {   // new
        struct LinuxSigaction { uint64_t handler; uint64_t flags;
                                void* restorer; uint64_t mask; };
        LinuxSigaction lsa;
        memcpy(&lsa, (const void*)(uintptr_t)a1, sizeof(lsa));
        e->handler = (void(*)(int, void*, void*))lsa.handler;
        e->flags = lsa.flags;
        e->domain = domains().current();
      }
      return 0;
    }
    case sys::rt_sigprocmask: return 0;
    case sys::sigaltstack: return 0;
    case sys::sched_setscheduler: return 0;
    case sys::sched_setaffinity: return 0;
    case sys::sched_getaffinity: {
      // 128-byte cpu mask, all 6 cores set
      uint64_t* mask = (uint64_t*)(uintptr_t)a2;
      if (mask && a3 >= 8) mask[0] = 0x3f;
      return (long)std::min<uint64_t>(a3, 128);
    }
    case sys::inotify_init1: return -ENOSYS;

    default:
      AOTD_LOGW("syscall", "ENOSYS nr=%ld (0x%lx) args=%lx,%lx,%lx",
                nr, nr, a0, a1, a2);
      return -ENOSYS;
  }
}

void syscallBridgeInit() {
  // Mount table defaults; the iOS bootstrapper (hardware_profiler ->
  // container_boot) overrides the container root with the app sandbox path.
  const char* cont = getenv("AOTDROID_CONTAINER");
  std::string root = cont ? cont : ".";
  paths().setContainerRoot(root);
  paths().addMount("/system", root + "/rootfs/system");
  paths().addMount("/vendor", root + "/rootfs/vendor");
  paths().addMount("/apex/com.android.runtime", root + "/rootfs/apex/com.android.runtime");
  paths().addMount("/apex", root + "/rootfs/apex");
  paths().addMount("/data", root + "/data");
  paths().addMount("/proc", root + "/proc");
  paths().addMount("/sys", root + "/sys");
  sigChainInstall();
  BinderEmu::get().initContexts();
}

#if !(defined(__aarch64__) && defined(AOTDROID_APPLE))
// On non-arm64-Apple targets there is no asm trampoline: the public entry
// declared in the header is a plain alias of the C dispatcher. (On arm64
// Apple builds the __asm__ trampoline above provides _aotdroid_syscall_bridge
// and tail-calls the dispatcher, preserving the Linux syscall register
// contract -- defining it again here would collide.)
extern "C" long aotdroid_syscall_bridge(long nr, long a0, long a1, long a2,
                                       long a3, long a4, long a5) {
  return aotdroid_syscall_c(nr, a0, a1, a2, a3, a4, a5);
}
#endif

}  // namespace aotdroid
