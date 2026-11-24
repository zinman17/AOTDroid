// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/jetsam/jetsam_guard.mm   (MODULE 7)
//  iPadOS Jetsam memory-pressure manager.
//
//  DISPATCH_SOURCE_TYPE_MEMORYPRESSURE (WARN / CRITICAL) triggers:
//    1. AMS-level trim: scheduleTrimMemory(TRIM_MEMORY_RUNNING_LOW/CRITICAL)
//       delivered to every registered app "process" via the binder snooper
//    2. purge all unpinned ashmem regions
//    3. madvise(MADV_DONTNEED) on cold .oat code pages
//    4. poll os_proc_available_memory(); ordered shutdown below the guard
// ===========================================================================
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import <os/proc.h>
#include "jetsam_guard.h"
#include "../runtime/container_boot.h"
#include "../runtime/vfs.h"

namespace aotdroid {

bool jetsamGuardInstall(NSString* containerPath) {
  dispatch_queue_t q = dispatch_queue_create("aotdroid.jetsam",
                                             DISPATCH_QUEUE_SERIAL);
  dispatch_source_t src = dispatch_source_create(
      DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
      DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, q);
  if (!src) return false;
  dispatch_source_set_event_handler(src, ^{
    uintptr_t flags = dispatch_source_get_data(src);
    if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) {
      AOTD_LOGW("jetsam", "CRITICAL pressure: available=%lld MB",
                (long long)(os_proc_available_memory() >> 20));
      aotdroid_memory_pressure_notify(2);
    } else if (flags & DISPATCH_MEMORYPRESSURE_WARN) {
      AOTD_LOGI("jetsam", "WARN pressure: available=%lld MB",
                (long long)(os_proc_available_memory() >> 20));
      aotdroid_memory_pressure_notify(1);
    }
  });
  dispatch_source_set_cancel_handler(src, ^{
    // ordered container shutdown if the source is cancelled at exit
  });
  dispatch_resume(src);
  (void)containerPath;

  // Watchdog thread: hard guard at 220 MB remaining. iOS kills without
  // warning once the per-app footprint cap is crossed, so we bail first
  // with an ordered domain shutdown (binder domains -> AMS -> SF).
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    for (;;) {
      uint64_t avail = (uint64_t)os_proc_available_memory();
      if (avail && avail < 220ull * 1024 * 1024) {
        AOTD_LOGE("jetsam", "below 220 MB guard: %lld MB -> emergency trim",
                  (long long)(avail >> 20));
        aotdroid_memory_pressure_notify(2);
      }
      [NSThread sleepForTimeInterval:0.25];
    }
  });
  return true;
}

long jetsamGuardCurrentAvailableBytes() {
  return (long)os_proc_available_memory();
}

}  // namespace aotdroid
