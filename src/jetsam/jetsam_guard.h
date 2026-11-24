// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/jetsam/jetsam_guard.h   (MODULE 7)
// ============================================================================
#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#else
#include <string>
#endif

namespace aotdroid {

// Installs the memory-pressure dispatch source + the 220 MB watchdog.
// containerPath points at the app container (used for logging/artifacts).
#ifdef __OBJC__
bool jetsamGuardInstall(NSString* containerPath);
#else
bool jetsamGuardInstall(const char* containerPath);
#endif
long jetsamGuardCurrentAvailableBytes();

}  // namespace aotdroid
