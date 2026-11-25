// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/profiler/hardware_profiler.h   (MODULE 1)
//  Darwin SoC & Memory Telemetry Probe + Adaptive AOSP Boot Selector.
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include <string>

namespace aotdroid {

// Apple CPU family ids (mach/machine.h); newer ones redefined locally so we
// build against older SDKs too.
constexpr uint32_t kCpuFamilyMonsoonMistral   = 0xe81e7ef6;  // A11
constexpr uint32_t kCpuFamilyVortexTempest    = 0x07d34b9f;  // A12
constexpr uint32_t kCpuFamilyLightningThunder = 0x462504d2;  // A13
constexpr uint32_t kCpuFamilyFirestormIcestorm = 0x1b588bb3; // A14 / M1
constexpr uint32_t kCpuFamilyAvalancheBlizzard = 0xda33d83d; // A15 / M2

enum class SocTier : uint8_t {
  Tier1_PreA14 = 0,    // A13 Lightning/Thunder and older (iPad 9, iPhone 11, SE2)
  Tier2_A14Plus = 1,   // A14 Firestorm/Icestorm, A15+, M1/M2 (iPad Air 4/5, iPad Pro)
  Unknown = 2
};

enum class AndroidTarget : uint8_t {
  Android_7_1_2_API25 = 0,
  Android_8_1_0_API27 = 1,
  Android_10_0_0_API29 = 2
};

enum class BootOverride : uint8_t { Auto = 0, Android71, Android81, Android10 };

struct SoCProfile {
  std::string machineModel;       // e.g. "iPad12,1"
  uint32_t cpuFamily = 0;
  const char* cpuFamilyName = "";
  SocTier tier = SocTier::Unknown;
  uint64_t memSize = 0;           // hw.memsize
  uint64_t procAvailable = 0;     // os_proc_available_memory()
  uint32_t pageSize = 16384;
  uint32_t coreCount = 0;
  bool hasFp16 = false, hasDotProd = false, hasLse = false;
  // display geometry (from the UIScreen, filled by the .mm layer)
  uint32_t screenW = 2160, screenH = 1620;
  uint32_t screenDpi = 264;
};

struct BootProfile {
  AndroidTarget target = AndroidTarget::Android_7_1_2_API25;
  uint32_t apiLevel = 25;
  const char* rootfsImage = "";      // container-relative pack name
  const char* oatFlavor = "088";     // OAT header version to verify
  const char* artHeapOpts = "";      // -Xmx/-Xms for zygote64/system_server
  const char* systemServerHeap = "";
  float fbScaleNum = 1.0f, fbScaleDen = 1.0f;   // SurfaceFlinger resolution
  uint32_t fbW = 2160, fbH = 1620;
  bool treble = false;               // vndbinder/hwservicemanager daemons
  uint32_t binderProto = 6;          // binder protocol version
  const char* profileTag = "android-7.1";
  const char* displayName = "Android 7.1.2 (Nougat)";
  // memory budget this profile must stay under (see docs/MEMORY_BUDGET.md)
  uint64_t rssBudgetMB = 0;
};

// Runtime profile selection API (implemented in hardware_profiler.mm).
bool profileSoC(SoCProfile* out);
BootProfile selectOptimalAndroidProfile(const SoCProfile& soc,
                                        BootOverride override_ = BootOverride::Auto);
const char* bootOverrideToString(BootOverride o);
BootOverride bootOverrideFromString(const char* s);

}  // namespace aotdroid

#ifdef __OBJC__
#import <Foundation/Foundation.h>

/// Objective-C facade over the profiler for the UIKit shell and CI tests.
@interface AOTDroidProfiler : NSObject
+ (instancetype)shared;
/// Raw SoC telemetry (sysctl + os_proc_available_memory).
- (NSDictionary*)probe;
/// Effective boot profile after override + RAM-headroom clamping.
- (NSDictionary*)selectProfile;
@end
#endif
