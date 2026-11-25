// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/linker/syscall_bridge.h   (MODULE 4)
//  Linux(arm64, asm-generic) -> Darwin syscall bridge. The AOT rewriter
//  redirects every `svc #0` site here. All critical translations required by
//  an unstripped AOSP user space are implemented:
//    futex            -> __ulock_wait / __ulock_wake
//    epoll_{create1,ctl,pwait} -> kqueue / kevent (+ virtual-fd wakeup pipe)
//    eventfd2         -> semaphore-counter + wakeup pipe
//    timerfd_{create,settime}  -> GCD dispatch source timers -> pipe
//    openat           -> PathTranslator + PackVFS + special nodes
//    getdents64       -> readdir packing with Linux dirent64 layout
//    mmap/mremap      -> Darwin mmap + registered relocation
//    prctl            -> PR_SET_VMA no-op / name routing
//    clone            -> ProcessDomain threads (fork-less isolator)
//    getrandom        -> CCRandomGenerateBytes / getentropy
//  plus rt_sigaction/sigaltstack/tgkill passthrough with a signal chain.
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include "../runtime/vfs.h"
#include "../runtime/binder_emu.h"

namespace aotdroid {

// Linux arm64 (asm-generic) syscall numbers -- asserted against the host's
// <asm-generic/unistd.h> in tests when available.
namespace sys {
constexpr long openat = 56, close = 57, pipe2 = 59, getdents64 = 61, lseek = 62,
  read = 63, write = 64, readv = 65, writev = 66, pread64 = 67, pwrite64 = 68,
  dup = 23, dup3 = 24, fcntl = 25, ioctl = 29, mkdirat = 34, unlinkat = 35,
  renameat = 38, faccessat = 48, chdir = 49, fchmodat = 53, readlinkat = 78,
  newfstatat = 79, fstat = 80,
  epoll_create1 = 20, epoll_ctl = 21, epoll_pwait = 22, eventfd2 = 19,
  timerfd_create = 85, timerfd_settime = 86, timerfd_gettime = 87,
  capget = 90, capset = 91, personality = 92, exit = 93, exit_group = 94,
  set_tid_address = 96, futex = 98, set_robust_list = 99, nanosleep = 101,
  clock_gettime = 113, clock_getres = 114, sched_setscheduler = 119,
  sched_setaffinity = 122, sched_getaffinity = 123, tgkill = 131,
  sigaltstack = 132, rt_sigaction = 134, rt_sigprocmask = 135,
  rt_sigreturn = 139, getcwd = 17, inotify_init1 = 26,
  setgid = 144, setuid = 146, getpid = 172, getppid = 173, getuid = 174,
  geteuid = 175, getgid = 176, getegid = 177, gettid = 178, prctl = 167,
  munmap = 215, mremap = 216, mmap = 222, mprotect = 226, msync = 227,
  madvise = 233, clone = 220, execve = 221, wait4 = 260, getrandom = 278,
  memfd_create = 279, ppoll = 73;
}  // namespace sys

// Linux constants the emulated user space expects.
constexpr long kO_RDONLY = 0, kO_WRONLY = 1, kO_RDWR = 2, kO_CREAT = 0100,
               kO_EXCL = 0200, kO_TRUNC = 01000, kO_APPEND = 02000,
               kO_NONBLOCK = 04000, kO_CLOEXEC = 02000000, kO_DIRECTORY = 0200000,
               kO_NOFOLLOW = 0400000, kO_LARGEFILE = 0100000;
constexpr long kF_GETFD = 1, kF_SETFD = 2, kF_GETFL = 3, kF_SETFL = 4,
               kFD_CLOEXEC = 1;
constexpr long kFUTEX_WAIT = 0, kFUTEX_WAKE = 1, kFUTEX_FD = 2,
               kFUTEX_REQUEUE = 3, kFUTEX_CMP_REQUEUE = 4,
               kFUTEX_PRIVATE_FLAG = 128, kFUTEX_WAIT_BITSET = 9,
               kFUTEX_WAKE_BITSET = 10;
constexpr long kEPOLL_CTL_ADD = 1, kEPOLL_CTL_DEL = 2, kEPOLL_CTL_MOD = 3;
constexpr long kPR_SET_NAME = 15, kPR_GET_NAME = 16, kPR_SET_VMA = 0x53564d41,
               kPR_SET_VMA_ANON_NAME = 0, kPR_SET_DUMPABLE = 4,
               kPR_GET_DUMPABLE = 3, kPR_SET_NO_NEW_PRIVS = 38;
constexpr long kCLOCK_MONOTONIC = 1, kCLOCK_REALTIME = 0, kCLOCK_BOOTTIME = 7;
constexpr long kMFD_CLOEXEC = 0x0001, kMFD_ALLOW_SEALING = 0x0002;
constexpr long kEFD_SEMAPHORE = 1, kEFD_CLOEXEC = kO_CLOEXEC;
constexpr long kTFD_CLOEXEC = kO_CLOEXEC, kTFD_NONBLOCK = kO_NONBLOCK;
constexpr long kSIG_BLOCK = 0, kSIG_UNBLOCK = 1, kSIG_SETMASK = 2;

struct LinuxTimespec { int64_t sec, nsec; };
struct LinuxTimeval { int64_t sec, usec; };
struct LinuxDirent64 {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};
struct LinuxStat64 {  // arm64 struct stat (128 bytes)
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid, st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t  st_size;
  int32_t  st_blksize;
  int32_t  __pad2;
  int64_t  st_blocks;
  int64_t  st_atime_sec, st_atime_nsec;
  int64_t  st_mtime_sec, st_mtime_nsec;
  int64_t  st_ctime_sec, st_ctime_nsec;
  uint32_t __unused4, __unused5;
};
struct EpollEvent { uint32_t events; uint64_t data; };

// ---------------------------------------------------------------------------
// Bridge entry points. `aotdroid_syscall_bridge` is the C symbol every
// syscall veneer calls; nr is in x8-style first arg, args a0..a5.
// Returns the Linux errno (negative) or the result.
// ---------------------------------------------------------------------------
extern "C" long aotdroid_syscall_bridge(long nr, long a0, long a1, long a2,
                                        long a3, long a4, long a5);
extern "C" void* aotdroid_get_android_tls_c();
extern "C" void  aotdroid_set_android_tls_c(void* tls);

// BRK trampoline table (out-of-range veneer fallback, ELF path).
void brkTrapRegister(size_t count, const void* patchSeg, size_t segLen);
long brkTrapDispatch(uint32_t idx, void* pc, uint64_t* regs /*x0..x30,sp*/);

// TLS block layout: [0] pthread_internal-ish slot mirror used by bionic.
struct AndroidTlsBlock {
  void* self;             // TLS_SELF: bionic expects tp->self == tp
  int64_t tid;
  uint64_t slots[61];     // bionic TLS slots (TLS_SLOT_* < 64)
  uint8_t  staticArea[4096];  // static TLS surplus for dlopen-style loads
};
AndroidTlsBlock* currentAndroidTls();

// Signal chain bookkeeping (per-domain handlers over the real Darwin handlers).
struct SigChainEntry {
  void (*handler)(int, void*, void*) = nullptr;   // Linux sa_handler with ucontext
  uint64_t flags = 0;
  uint32_t domain = kInvalidDomain;
};
void sigChainInstall();                       // install Darwin master handlers
SigChainEntry* sigChainGet(int sig);

// init: called once before any Android code runs.
void syscallBridgeInit();

// Exec image registry: execve() re-homes a thread into a pre-linked image.
struct ExecImage {
  std::string path;
  void* entry = nullptr;      // function(void*) style trampoline
  void* ctx = nullptr;
  std::vector<std::string> needed;
};
void execRegisterImage(const ExecImage& img);

// ---------------------------------------------------------------------------
// Linux futex <-> Darwin ulock
// ---------------------------------------------------------------------------
long futexWait(uint32_t* uaddr, uint32_t expected, const LinuxTimespec* ts,
               uint32_t bitset = 0xffffffff);
long futexWake(uint32_t* uaddr, uint32_t count, uint32_t bitset = 0xffffffff);

// epoll emulation over kqueue + virtual-fd poll functions
int  epollCreate1(int flags);
int  epollCtl(int epfd, int op, int fd, const EpollEvent* ev);
int  epollWait(int epfd, EpollEvent* out, int maxEvents, int timeoutMs);

// eventfd
int  eventfdCreate(unsigned initval, int flags);
// timerfd
int  timerfdCreate(int clockid, int flags);
int  timerfdSettime(int fd, int flags, const LinuxTimespec* itValue,
                    const LinuxTimespec* itInterval, LinuxTimespec* oldValue);

}  // namespace aotdroid
