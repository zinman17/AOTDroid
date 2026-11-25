// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/platform/sys_compat.h
//  Cross-platform compatibility surface for the portable core.
//
//  The portable core (modules 2/3/4 + graphics-core + input + boot) compiles
//  on Darwin/iOS (production target) and on Linux/macOS hosts (CI unit tests,
//  host-side AOT rewriter tool). All Darwin-specific kernel interfaces used by
//  the portable core are isolated here. On non-Apple hosts the shims are
//  syntax/ABI stubs that return -ENOSYS; they exist only so the host build
//  links the test binaries. Device builds always take the __APPLE__ path.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <algorithm>

#if defined(__APPLE__)
  #include <sys/types.h>
  #include <sys/mman.h>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <sys/event.h>      // kqueue / kevent  (epoll + eventfd emulation)
  #if defined(__has_include)
    #if __has_include(<sys/ulock.h>)
      #include <sys/ulock.h>  // __ulock_wait/__ulock_wake (futex emulation)
    #endif
  #endif
  #ifndef __ULOCK_DECLARED
    // Newer Xcode SDKs no longer ship <sys/ulock.h>, but the ulock entry
    // points are a stable C ABI in libSystem -- declare them ourselves.
    #define __ULOCK_DECLARED 1
    extern "C" {
    int __ulock_wait(uint32_t operation, void* addr, uint64_t value,
                     uint32_t timeout);
    int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
    }
    #ifndef UL_COMPARE_AND_WAIT
      #define UL_COMPARE_AND_WAIT      1
    #endif
    #ifndef UL_COMPARE_AND_WAIT64
      #define UL_COMPARE_AND_WAIT64    5
    #endif
    #ifndef ULF_WAKE_ALL
      #define ULF_WAKE_ALL             0x0100
    #endif
  #endif
  #include <unistd.h>
  #include <fcntl.h>
  #include <pthread.h>
  #include <dirent.h>
  #define AOTDROID_APPLE   1
  #define AOTDROID_HAVE_KQUEUE 1
  #define AOTDROID_HAVE_ULOCK  1
#else
  #include <sys/mman.h>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <sys/uio.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <pthread.h>
  #include <dirent.h>

  // ---- kqueue surface (host stubs; never used at runtime on Linux) ----
  #define EVFILT_READ     (-1)
  #define EVFILT_WRITE    (-2)
  #define EVFILT_USER     (-9)
  #define EV_ADD          0x0001
  #define EV_DELETE       0x0002
  #define EV_ENABLE       0x0004
  #define EV_DISABLE      0x0008
  #define EV_ONESHOT      0x0010
  #define EV_CLEAR        0x0020
  #define EV_RECEIPT      0x0040
  #define EV_ERROR        0x4000
  #define EV_EOF          0x8000
  #define NOTE_TRIGGER    0x01000000
  struct kevent {
    uintptr_t ident; uint16_t filter; uint16_t flags; uint32_t fflags;
    intptr_t data; void* udata;
  };
  inline int kqueue(void) { errno = ENOSYS; return -1; }
  inline int kevent(int, const struct kevent*, int, struct kevent*, int,
                    const struct timespec*) { errno = ENOSYS; return -1; }

  // ---- ulock surface (host stubs) ----
  #define UL_COMPARE_AND_WAIT      1
  #define UL_COMPARE_AND_WAIT64    5
  #define ULF_WAKE_ALL             0x0100
  inline int __ulock_wait(uint32_t, void*, uint64_t, uint32_t) { errno = ENOSYS; return -1; }
  inline int __ulock_wake(uint32_t, void*, uint64_t) { errno = ENOSYS; return -1; }
#endif

namespace aotdroid {

// ---------------------------------------------------------------------------
// Logging: routed to os_log on device builds, stderr on host builds.
// ---------------------------------------------------------------------------
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline void log_write(LogLevel lvl, const char* tag, const char* fmt, ...) {
  static const char* kNames[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
  char buf[1024];
  int n = snprintf(buf, sizeof(buf), "[aotdroid][%s][%s] ", kNames[(int)lvl], tag);
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
  va_end(ap);
  fputs(buf, stderr); fputc('\n', stderr);
}
#define AOTD_LOG(lvl, tag, ...) ::aotdroid::log_write(lvl, tag, __VA_ARGS__)
#define AOTD_LOGI(tag, ...) AOTD_LOG(::aotdroid::LogLevel::Info,  tag, __VA_ARGS__)
#define AOTD_LOGW(tag, ...) AOTD_LOG(::aotdroid::LogLevel::Warn,  tag, __VA_ARGS__)
#define AOTD_LOGE(tag, ...) AOTD_LOG(::aotdroid::LogLevel::Error, tag, __VA_ARGS__)

// Linux errno not present on Darwin.
#ifndef ERESTARTSYS
  #define ERESTARTSYS 512
#endif

// Timespec helpers (Darwin lacks timespecsub in C++ context).
inline struct timespec ts_add_ns(struct timespec ts, int64_t ns) {
  ts.tv_sec += (time_t)(ns / 1000000000LL);
  ts.tv_nsec += (long)(ns % 1000000000LL);
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
  return ts;
}
inline int64_t ts_to_us(struct timespec ts) {
  return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

}  // namespace aotdroid
