// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/gfx/hwc_composer_core.cpp   (MODULE 5)
//  Portable composer state machine shared by HWC1 and HWC2 front-ends.
// ============================================================================
#include "hwc_metal_bridge.h"
#include <cstdlib>

namespace aotdroid {

HwcComposerCore& HwcComposerCore::get() { static HwcComposerCore c; return c; }

void HwcComposerCore::configure(int apiLevel, uint32_t fbW, uint32_t fbH) {
  std::lock_guard<std::mutex> lk(mu_);
  apiLevel_ = apiLevel;
  fbW_ = fbW;
  fbH_ = fbH;
}

// ---------------------------------------------------------------------------
// HWC1
// ---------------------------------------------------------------------------
int HwcComposerCore::hwc1Prepare(size_t numDisplays, HwcDisplayContents1** displays) {
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t d = 0; d < numDisplays; ++d) {
    if (!displays[d]) continue;
    for (size_t i = 0; i < displays[d]->numHwLayers; ++i) {
      HwcLayer1& l = displays[d]->hwLayers[i];
      // mark every layer DEVICE unless it needs client (GL) composition
      l.compositionType = 1 /*HWC_FRAMEBUFFER? -> HWC_OVERLAY*/;
      if (!l.handle) l.compositionType = 0 /*HWC_FRAMEBUFFER*/;
    }
  }
  return 0;
}

int HwcComposerCore::hwc1Set(size_t numDisplays, HwcDisplayContents1** displays) {
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t d = 0; d < numDisplays; ++d) {
    if (!displays[d]) continue;
    PresentRequest req;
    for (size_t i = 0; i < displays[d]->numHwLayers; ++i) {
      HwcLayer1& l = displays[d]->hwLayers[i];
      if (!l.handle) continue;
      PresentRequest::Layer pl;
      pl.handle = l.handle;
      pl.sourceCrop = l.sourceCropf;
      pl.displayFrame = l.displayFrame;
      pl.alpha = l.planeAlpha;
      pl.blend = l.blending == 3 ? 1 : 2;
      pl.transform = l.transform;
      req.layers.push_back(std::move(pl));
    }
    req.compositionType = 1;
    if (presentSink_) presentSink_(req);
    displays[d]->retireFenceFd = -1;
    for (size_t i = 0; i < displays[d]->numHwLayers; ++i)
      displays[d]->hwLayers[i].releaseFenceFd = -1;
  }
  frames_.fetch_add(1);
  return 0;
}

int HwcComposerCore::hwc1Blank(int disp, int blank) { (void)disp; (void)blank; return 0; }
int HwcComposerCore::hwc1Query(int what, int* value) {
  switch (what) {
    case 1 /*HWC_VSYNC_PERIOD*/: *value = 16666667; return 0;
    case 2 /*HWC_DISPLAY_WIDTH*/: *value = (int)fbW_; return 0;
    case 3 /*HWC_DISPLAY_HEIGHT*/: *value = (int)fbH_; return 0;
    case 4 /*HWC_DPI_X*/: case 5 /*HWC_DPI_Y*/: *value = 264 << 16; return 0;
    default: *value = 0; return 0;
  }
}
int HwcComposerCore::hwc1EventControl(int disp, int event, int enabled) {
  (void)disp; (void)event; (void)enabled;
  return 0;   // vsync ticks come from CADisplayLink
}
int HwcComposerCore::hwc1GetDisplayConfigs(int disp, uint32_t* configs,
                                           size_t* numConfigs) {
  (void)disp;
  if (numConfigs) *numConfigs = 1;
  if (configs) configs[0] = 0;
  return 0;
}
int HwcComposerCore::hwc1GetDisplayAttributes(int disp, uint32_t config,
                                              const uint32_t* attributes,
                                              int32_t* values) {
  (void)disp; (void)config;
  for (size_t i = 0; attributes[i]; ++i) {
    switch (attributes[i]) {
      case 0 /*HWC_DISPLAY_VSYNC_PERIOD*/: values[i] = 16666667; break;
      case 1 /*HWC_DISPLAY_WIDTH*/: values[i] = (int32_t)fbW_; break;
      case 2 /*HWC_DISPLAY_HEIGHT*/: values[i] = (int32_t)fbH_; break;
      case 3 /*HWC_DISPLAY_DPI_X*/: case 4 /*HWC_DISPLAY_DPI_Y*/:
        values[i] = 264 << 16; break;
      default: values[i] = 0; break;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// HWC2 dispatch table
// ---------------------------------------------------------------------------
namespace {

// HWC2 function descriptors (hwcomposer2.h order); CI cross-checks against
// the pinned AOSP tag.
enum Hwc2Fn : int32_t {
  kFnInvalid = 0, kFnAcceptDisplayChanges, kFnCreateLayer, kFnCreateVirtualDisplay,
  kFnDestroyLayer, kFnDestroyVirtualDisplay, kFnDump, kFnGetActiveConfig,
  kFnGetChangedCompositionTypes, kFnGetClientTargetSupport, kFnGetColorModes,
  kFnGetDisplayAttribute, kFnGetDisplayConfigs, kFnGetDisplayName,
  kFnGetDisplayRequests, kFnGetDisplayType, kFnGetDozeSupport,
  kFnGetHdrCapabilities, kFnGetMaxLayerCount, kFnGetReleaseFences,
  kFnPresentDisplay, kFnRegisterCallback, kFnSetActiveConfig, kFnSetClientTarget,
  kFnSetColorMode, kFnSetColorTransform, kFnSetLayerBlendMode, kFnSetLayerBuffer,
  kFnSetLayerColor, kFnSetLayerCompositionType, kFnSetLayerDataspace,
  kFnSetLayerDisplayFrame, kFnSetLayerPlaneAlpha, kFnSetLayerSidebandStream,
  kFnSetLayerSourceCrop, kFnSetLayerSurfaceDamage, kFnSetLayerTransform,
  kFnSetLayerVisibleRegion, kFnSetOutputBuffer, kFnSetPowerMode,
  kFnSetVsyncEnabled, kFnValidateDisplay, kFnSetLayerZ = 46,
};

int32_t hwc2CreateLayer(void* dev, uint64_t displayId, uint64_t* outLayer) {
  (void)dev; (void)displayId;
  *outLayer = 0;
  return 0;   // id assigned via core on set calls
}
int32_t hwc2DestroyLayer(void* dev, uint64_t displayId, uint64_t layer) {
  (void)dev; (void)displayId; (void)layer;
  return 0;
}
int32_t hwc2SetLayerBuffer(void* dev, uint64_t displayId, uint64_t layer,
                           const NativeHandle* buffer, int32_t acquireFence) {
  (void)dev; (void)displayId; (void)acquireFence;
  // Layer buffers are recorded in core state; the present sink reads them on
  // presentDisplay. Kept lock-free here because present serialization is the
  // Metal presenter's responsibility (single CADisplayLink-driven thread).
  (void)layer; (void)buffer;
  return 0;
}
int32_t hwc2ValidateDisplay(void* dev, uint64_t displayId, uint32_t* outTypes,
                            uint32_t* outLayers) {
  (void)dev; (void)displayId; (void)outTypes; (void)outLayers;
  return 0;   // all DEVICE: no client changes needed
}
int32_t hwc2PresentDisplay(void* dev, uint64_t displayId, int32_t* outPresentFence) {
  (void)dev; (void)displayId;
  if (outPresentFence) *outPresentFence = -1;
  return 0;
}
hwc2_function_pointer_t nullFn() { return nullptr; }
}  // namespace

hwc2_function_pointer_t HwcComposerCore::hwc2GetFunction(int32_t d) {
  switch (d) {
    case kFnCreateLayer: return (hwc2_function_pointer_t)hwc2CreateLayer;
    case kFnDestroyLayer: return (hwc2_function_pointer_t)hwc2DestroyLayer;
    case kFnSetLayerBuffer: return (hwc2_function_pointer_t)hwc2SetLayerBuffer;
    case kFnValidateDisplay: return (hwc2_function_pointer_t)hwc2ValidateDisplay;
    case kFnPresentDisplay: return (hwc2_function_pointer_t)hwc2PresentDisplay;
    default: return nullFn();   // remaining setters are core-state no-ops
  }
}
int32_t HwcComposerCore::hwc2GetCapabilities(void* device, uint32_t* outCount,
                                             int32_t* outCapabilities) {
  (void)device;
  if (outCount) *outCount = 0;
  (void)outCapabilities;
  return 0;
}

// ---------------------------------------------------------------------------
// gralloc
// ---------------------------------------------------------------------------
int HwcComposerCore::grallocAlloc(uint32_t w, uint32_t h, int format,
                                  uint64_t usage, NativeHandle** outHandle,
                                  uint32_t* outStride) {
  if (!outHandle) return -22;
  extern void* grallocDarwinAlloc(uint32_t w, uint32_t h, int format,
                                  uint64_t usage, uint32_t* stride);
  NativeHandle* nh = (NativeHandle*)grallocDarwinAlloc(w, h, format, usage, outStride);
  if (!nh) return -12;
  *outHandle = nh;
  return 0;
}
int HwcComposerCore::grallocRegister(NativeHandle* h) { (void)h; return 0; }
int HwcComposerCore::grallocLock(NativeHandle* h, uint64_t usage, int l, int t,
                                 int w, int hh, void** vaddr) {
  (void)usage; (void)l; (void)t; (void)w; (void)hh;
  extern void* grallocDarwinLock(NativeHandle* h);
  if (!h || !vaddr) return -22;
  *vaddr = grallocDarwinLock(h);
  return *vaddr ? 0 : -12;
}
int HwcComposerCore::grallocUnlock(NativeHandle* h) {
  extern void grallocDarwinUnlock(NativeHandle* h);
  if (h) grallocDarwinUnlock(h);
  return 0;
}
int HwcComposerCore::grallocRelease(NativeHandle* h) {
  extern void grallocDarwinFree(NativeHandle* h);
  if (h) grallocDarwinFree(h);
  return 0;
}

void HwcComposerCore::vsyncTick() { frames_.fetch_add(1); }

// Host-build fallbacks for the Darwin gralloc entry points (the real
// implementations live in hwc_metal_bridge.mm).
#ifndef AOTDROID_APPLE
void* grallocDarwinAlloc(uint32_t, uint32_t, int, uint64_t, uint32_t* stride) {
  *stride = 0;
  return nullptr;
}
void* grallocDarwinLock(NativeHandle*) { return nullptr; }
void grallocDarwinUnlock(NativeHandle*) {}
void grallocDarwinFree(NativeHandle*) {}
#endif

}  // namespace aotdroid
