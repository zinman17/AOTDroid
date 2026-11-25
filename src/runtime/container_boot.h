// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/container_boot.h
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include "../runtime/binder_emu.h"

namespace aotdroid {

struct BootConfig {
  uint32_t apiLevel = 25;
  const char* profileTag = "android-7.1";
  const char* deviceModel = "AOTDroid";
  std::string containerRoot;
  std::vector<std::pair<std::string, std::string>> extraProperties;
};

struct BootResult {
  bool ok = false;
  std::string error;
  DomainId initDomain = kInvalidDomain;
  size_t propertyBytes = 0;
};

BootResult containerBoot(const BootConfig& cfg);
long aotdroid_memory_pressure_notify(int level);   // 0 normal 1 warn 2 critical

}  // namespace aotdroid
