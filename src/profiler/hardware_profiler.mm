// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/profiler/hardware_profiler.mm   (MODULE 1)
//  Darwin SoC & Memory Telemetry Probe (sysctl + os_proc_available_memory)
//  and the adaptive AOSP boot-selector decision matrix.
//
//  Runs at startup BEFORE mounting any rootfs pack or allocating ART heaps.
// ============================================================================
#import <Foundation/Foundation.h>
#import <os/proc.h>
#import <sys/sysctl.h>
#import <sys/types.h>
#import <dispatch/dispatch.h>
#include "hardware_profiler.h"

namespace aotdroid {

// ---------------------------------------------------------------------------
// sysctl helpers
// ---------------------------------------------------------------------------
namespace {
std::string sysctlString(const char* name) {
  char buf[256] = {0};
  size_t len = sizeof(buf) - 1;
  if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return "";
  return std::string(buf, strnlen(buf, len));
}
uint64_t sysctlU64(const char* name) {
  uint64_t v = 0;
  size_t len = sizeof(v);
  if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
  return v;
}
bool sysctlBool(const char* name) {
  int v = 0; size_t len = sizeof(v);
  if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return false;
  return v != 0;
}

struct KnownModel { const char* model; SocTier tier; const char* marketing; };
const KnownModel kModels[] = {
  // Tier 1 (A13 / A12 / A11 class)
  {"iPad12,1", SocTier::Tier1_PreA14, "iPad 9th Gen (A13)"},
  {"iPad12,2", SocTier::Tier1_PreA14, "iPad 9th Gen Cellular (A13)"},
  {"iPad11,6", SocTier::Tier1_PreA14, "iPad 8th Gen (A12)"},
  {"iPad11,7", SocTier::Tier1_PreA14, "iPad 8th Gen Cellular (A12)"},
  {"iPad7,11", SocTier::Tier1_PreA14, "iPad 7th Gen (A10)"},
  {"iPad7,12", SocTier::Tier1_PreA14, "iPad 7th Gen Cellular (A10)"},
  {"iPhone12,1", SocTier::Tier1_PreA14, "iPhone 11 (A13)"},
  {"iPhone12,3", SocTier::Tier1_PreA14, "iPhone 11 Pro (A13)"},
  {"iPhone12,5", SocTier::Tier1_PreA14, "iPhone 11 Pro Max (A13)"},
  {"iPhone12,8", SocTier::Tier1_PreA14, "iPhone SE 2nd Gen (A13)"},
  // Tier 2 (A14 / A15 / M1 / M2 class)
  {"iPad13,1", SocTier::Tier2_A14Plus, "iPad Air 4 (A14)"},
  {"iPad13,2", SocTier::Tier2_A14Plus, "iPad Air 4 Cellular (A14)"},
  {"iPad13,16", SocTier::Tier2_A14Plus, "iPad Air 5 (M1)"},
  {"iPad13,17", SocTier::Tier2_A14Plus, "iPad Air 5 Cellular (M1)"},
  {"iPad13,4", SocTier::Tier2_A14Plus, "iPad Pro 11in 4th Gen (M2)"},
  {"iPad13,5", SocTier::Tier2_A14Plus, "iPad Pro 11in (M1)"},
  {"iPad13,6", SocTier::Tier2_A14Plus, "iPad Pro 11in (M1)"},
  {"iPad13,7", SocTier::Tier2_A14Plus, "iPad Pro 11in (M1)"},
  {"iPad13,8", SocTier::Tier2_A14Plus, "iPad Pro 12.9in (M1)"},
  {"iPad13,9", SocTier::Tier2_A14Plus, "iPad Pro 12.9in (M1)"},
  {"iPad13,10", SocTier::Tier2_A14Plus, "iPad Pro 12.9in (M1)"},
  {"iPad13,18", SocTier::Tier2_A14Plus, "iPad 10th Gen (A14)"},
  {"iPad13,19", SocTier::Tier2_A14Plus, "iPad 10th Gen Cellular (A14)"},
  {"iPad14,1", SocTier::Tier2_A14Plus, "iPad mini 6 (A15)"},
  {"iPad14,2", SocTier::Tier2_A14Plus, "iPad mini 6 Cellular (A15)"},
};
}  // namespace

// ---------------------------------------------------------------------------
// SoC probe
// ---------------------------------------------------------------------------
bool profileSoC(SoCProfile* out) {
  if (!out) return false;
  SoCProfile p;
  p.machineModel = sysctlString("hw.machine");
  p.cpuFamily = (uint32_t)sysctlU64("hw.cpufamily");
  p.memSize = sysctlU64("hw.memsize");
  p.pageSize = (uint32_t)sysctlU64("hw.pagesize");
  if (p.pageSize == 0) p.pageSize = 16384;
  p.coreCount = (uint32_t)sysctlU64("hw.ncpu");
  if (p.coreCount == 0) p.coreCount = (uint32_t)sysctlU64("hw.logicalcpu");
  p.procAvailable = (uint64_t)os_proc_available_memory();
  p.hasFp16 = sysctlBool("hw.optional.armv8_2_fp16");
  p.hasDotProd = sysctlBool("hw.optional.armv8_2_dotprod");
  p.hasLse = sysctlBool("hw.optional.armv8_1_atomics");

  switch (p.cpuFamily) {
    case kCpuFamilyMonsoonMistral:
      p.cpuFamilyName = "A11 Monsoon/Mistral"; p.tier = SocTier::Tier1_PreA14; break;
    case kCpuFamilyVortexTempest:
      p.cpuFamilyName = "A12 Vortex/Tempest"; p.tier = SocTier::Tier1_PreA14; break;
    case kCpuFamilyLightningThunder:
      p.cpuFamilyName = "A13 Lightning/Thunder"; p.tier = SocTier::Tier1_PreA14; break;
    case kCpuFamilyFirestormIcestorm:
      p.cpuFamilyName = "A14/M1 Firestorm/Icestorm"; p.tier = SocTier::Tier2_A14Plus; break;
    case kCpuFamilyAvalancheBlizzard:
      p.cpuFamilyName = "A15/M2 Avalanche/Blizzard"; p.tier = SocTier::Tier2_A14Plus; break;
    default: {
      // Unknown family: classify by model table, then by RAM heuristic.
      p.cpuFamilyName = "unknown";
      p.tier = SocTier::Unknown;
      for (const auto& m : kModels) {
        if (p.machineModel == m.model) { p.tier = m.tier; break; }
      }
      if (p.tier == SocTier::Unknown) {
        // Apple Silicon is at least A14 if the kernel page size is 16K on iOS
        // and the model id is >= iPad13,*; otherwise assume Tier1 (safe).
        p.tier = p.memSize >= (4ull << 30) ? SocTier::Tier2_A14Plus
                                           : SocTier::Tier1_PreA14;
      }
      break;
    }
  }
  *out = p;
  return true;
}

// ---------------------------------------------------------------------------
// Decision matrix
// ---------------------------------------------------------------------------
static BootProfile makeProfile(AndroidTarget t, const SoCProfile& soc) {
  BootProfile b;
  b.target = t;
  switch (t) {
    case AndroidTarget::Android_7_1_2_API25:
      b.apiLevel = 25;
      b.rootfsImage = "rootfs-android-7.1.aotpack";
      b.oatFlavor = "088";
      b.profileTag = "android-7.1";
      b.displayName = "Android 7.1.2 (Nougat) - ultra-stable";
      b.treble = false;
      b.binderProto = 6;
      b.rssBudgetMB = 850;
      if (soc.tier == SocTier::Tier1_PreA14 && soc.memSize <= (3ull << 30)) {
        // A13 3 GB: 0.75x framebuffer, tight heaps
        b.fbScaleNum = 3; b.fbScaleDen = 4;
        b.fbW = (soc.screenW * 3) / 4;      // 2160 -> 1620
        b.fbH = (soc.screenH * 3) / 4;      // 1620 -> 1215
        b.artHeapOpts = "-Xms128m -Xmx256m";
        b.systemServerHeap = "-Xms96m -Xmx384m";
      } else {
        b.artHeapOpts = "-Xms192m -Xmx320m";
        b.systemServerHeap = "-Xms128m -Xmx448m";
      }
      break;
    case AndroidTarget::Android_8_1_0_API27:
      b.apiLevel = 27;
      b.rootfsImage = "rootfs-android-8.1.aotpack";
      b.oatFlavor = "131";
      b.profileTag = "android-8.1";
      b.displayName = "Android 8.1.0 (Oreo) - balanced (Treble split off)";
      b.treble = false;    // vndbinder/hwservicemanager disabled on Tier 1
      b.binderProto = 7;
      b.rssBudgetMB = 1100;
      b.artHeapOpts = "-Xms160m -Xmx320m";
      b.systemServerHeap = "-Xms112m -Xmx448m";
      if (soc.tier == SocTier::Tier1_PreA14 && soc.memSize <= (3ull << 30)) {
        b.fbScaleNum = 3; b.fbScaleDen = 4;
        b.fbW = (soc.screenW * 3) / 4;
        b.fbH = (soc.screenH * 3) / 4;
      }
      break;
    case AndroidTarget::Android_10_0_0_API29:
      b.apiLevel = 29;
      b.rootfsImage = "rootfs-android-10.aotpack";
      b.oatFlavor = "170";
      b.profileTag = "android-10";
      b.displayName = "Android 10.0.0 (Q) - full unstripped + APEX + HIDL";
      b.treble = true;
      b.binderProto = 8;
      b.rssBudgetMB = 1900;
      b.artHeapOpts = "-Xms256m -Xmx512m";
      b.systemServerHeap = "-Xms192m -Xmx640m";
      b.fbW = soc.screenW; b.fbH = soc.screenH;
      break;
  }
  return b;
}

BootProfile selectOptimalAndroidProfile(const SoCProfile& soc,
                                        BootOverride ov) {
  AndroidTarget t;
  if (ov == BootOverride::Auto) {
    // ---- automatic matrix ----------------------------------------------
    // Tier 1 + 3 GB: Android 7.1.2 default; Android 8.1 only if the free
    // headroom can carry it beneath the ~1.8 GB Jetsam threshold.
    // Tier 2 (A14/M1+, 4-16 GB): Android 10 full user space.
    if (soc.tier == SocTier::Tier2_A14Plus) {
      t = AndroidTarget::Android_10_0_0_API29;
    } else {
      const uint64_t headroomBytes = soc.procAvailable
        ? soc.procAvailable : soc.memSize / 2;
      if (soc.memSize <= (3ull << 30)) {
        // iPad 9 class: default to 7.1.2 (ultra-stable ~650-850 MB).
        // 8.1 (850-1100 MB) only when >= 1.35 GB is actually available.
        t = headroomBytes >= 1350ull * 1024 * 1024
              ? AndroidTarget::Android_8_1_0_API27
              : AndroidTarget::Android_7_1_2_API25;
      } else {
        // >= 4 GB RAM but pre-A14 SoC: 8.1 balanced by default.
        t = AndroidTarget::Android_8_1_0_API27;
      }
    }
  } else {
    t = ov == BootOverride::Android71 ? AndroidTarget::Android_7_1_2_API25
      : ov == BootOverride::Android81 ? AndroidTarget::Android_8_1_0_API27
                                      : AndroidTarget::Android_10_0_0_API29;
  }

  BootProfile b = makeProfile(t, soc);

  // Safety clamp: never auto-select a profile whose RSS budget exceeds the
  // process's available memory minus a 256 MB guard (Jetsam pre-kill margin).
  const uint64_t avail = soc.procAvailable ? soc.procAvailable
                                           : (soc.memSize * 3) / 4;
  const uint64_t budget = b.rssBudgetMB * 1024 * 1024;
  if (budget + (256ull << 20) > avail) {
    if (t != AndroidTarget::Android_7_1_2_API25) {
      AOTD_LOGW("profiler",
                "profile %s budget %llu MB exceeds headroom %llu MB; downgrading",
                b.profileTag, (unsigned long long)b.rssBudgetMB,
                (unsigned long long)(avail >> 20));
      b = makeProfile(AndroidTarget::Android_7_1_2_API25, soc);
      if (ov != BootOverride::Auto) {
        AOTD_LOGW("profiler",
                  "manual override kept, enabling aggressive madvise eviction");
        b.artHeapOpts = "-Xms96m -Xmx192m";
        b.systemServerHeap = "-Xms64m -Xmx256m";
      }
    }
  }
  return b;
}

const char* bootOverrideToString(BootOverride o) {
  switch (o) {
    case BootOverride::Android71: return "android-7.1";
    case BootOverride::Android81: return "android-8.1";
    case BootOverride::Android10: return "android-10";
    default: return "auto";
  }
}
BootOverride bootOverrideFromString(const char* s) {
  if (!s) return BootOverride::Auto;
  std::string v(s);
  if (v == "android-7.1") return BootOverride::Android71;
  if (v == "android-8.1") return BootOverride::Android81;
  if (v == "android-10") return BootOverride::Android10;
  return BootOverride::Auto;
}

}  // namespace aotdroid

// ---------------------------------------------------------------------------
// Objective-C wrapper used by the app shell + jetsam guard.
// ---------------------------------------------------------------------------
@implementation AOTDroidProfiler : NSObject

+ (instancetype)shared {
  static AOTDroidProfiler* p = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ p = [[AOTDroidProfiler alloc] init]; });
  return p;
}

- (NSDictionary*)probe {
  aotdroid::SoCProfile soc;
  if (!aotdroid::profileSoC(&soc)) return @{};
  return @{
    @"model": [NSString stringWithUTF8String:soc.machineModel.c_str()],
    @"cpuFamily": @(soc.cpuFamily),
    @"cpuFamilyName": [NSString stringWithUTF8String:soc.cpuFamilyName],
    @"tier": @(soc.tier == aotdroid::SocTier::Tier2_A14Plus ? 2 : 1),
    @"memSize": @(soc.memSize),
    @"procAvailable": @(soc.procAvailable),
    @"pageSize": @(soc.pageSize),
    @"cores": @(soc.coreCount),
    @"fp16": @(soc.hasFp16),
    @"dotprod": @(soc.hasDotProd),
    @"lse": @(soc.hasLse),
  };
}

// Reads: NSUserDefaults "AOTDroidBootOverride" -> Info.plist "BootSelector"
// -> auto. Validates RAM headroom and returns the effective profile.
- (NSDictionary*)selectProfile {
  aotdroid::SoCProfile soc;
  if (!aotdroid::profileSoC(&soc)) return @{};

  NSString* ovStr = [[NSUserDefaults standardUserDefaults]
      stringForKey:@"AOTDroidBootOverride"];
  if (!ovStr) {
    ovStr = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"BootSelector"];
  }
  aotdroid::BootOverride ov = aotdroid::bootOverrideFromString(
      ovStr ? ovStr.UTF8String : "auto");

  aotdroid::BootProfile bp = aotdroid::selectOptimalAndroidProfile(soc, ov);
  return @{
    @"apiLevel": @(bp.apiLevel),
    @"rootfsImage": [NSString stringWithUTF8String:bp.rootfsImage],
    @"oatFlavor": [NSString stringWithUTF8String:bp.oatFlavor],
    @"profileTag": [NSString stringWithUTF8String:bp.profileTag],
    @"displayName": [NSString stringWithUTF8String:bp.displayName],
    @"artHeapOpts": [NSString stringWithUTF8String:bp.artHeapOpts],
    @"systemServerHeap": [NSString stringWithUTF8String:bp.systemServerHeap],
    @"fbW": @(bp.fbW),
    @"fbH": @(bp.fbH),
    @"treble": @(bp.treble),
    @"rssBudgetMB": @(bp.rssBudgetMB),
  };
}

@end
