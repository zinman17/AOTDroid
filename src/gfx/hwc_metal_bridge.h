// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/gfx/hwc_metal_bridge.h   (MODULE 5)
//  Adaptive Hardware Composer Bridge: HWC 1.5 (Android 7.1) + HWC 2.0
//  (Android 8.1/10) over IOSurface-backed gralloc buffers presented onto a
//  CAMetalLayer. GLES/EGL is served by ANGLE (OpenGL ES -> Metal).
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include <functional>

namespace aotdroid {

// HAL pixel formats (system/graphics.h)
constexpr int kHAL_PIXEL_FORMAT_RGBA_8888 = 1;
constexpr int kHAL_PIXEL_FORMAT_RGBX_8888 = 2;
constexpr int kHAL_PIXEL_FORMAT_RGB_888 = 3;
constexpr int kHAL_PIXEL_FORMAT_BGRA_8888 = 4;
constexpr int kHAL_PIXEL_FORMAT_YCbCr_420_888 = 0x23;
constexpr int kHAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED = 0x22;

// hw_module_t / hw_device_t constants
constexpr uint32_t kHwModuleTag = 0x48574D54;   // 'H','W','M','T'
constexpr uint32_t kHwDeviceTag = 0x48444E56;   // 'H','D','N','V'
constexpr uint32_t kHwModuleMakeVersion(uint16_t major, uint16_t minor) {
  return ((uint32_t)major << 16) | minor;
}
constexpr uint32_t kGrallocModuleApiVersion0 = kHwModuleMakeVersion(0, 4);
constexpr uint32_t kHwcModuleApiVersion1_5 = kHwModuleMakeVersion(1, 5);
constexpr uint32_t kHwcModuleApiVersion2_0 = kHwModuleMakeVersion(2, 0);

// gralloc usage bits
constexpr uint64_t kGRALLOC_USAGE_SW_READ_OFTEN = 0x3;
constexpr uint64_t kGRALLOC_USAGE_SW_WRITE_OFTEN = 0x30;
constexpr uint64_t kGRALLOC_USAGE_HW_FB = 0x00001000;
constexpr uint64_t kGRALLOC_USAGE_HW_RENDER = 0x00000200;
constexpr uint64_t kGRALLOC_USAGE_HW_TEXTURE = 0x00000100;
constexpr uint64_t kGRALLOC_USAGE_HW_COMPOSER = 0x00000800;

struct NativeHandle { int fd; int magic; int version; uint32_t width, height, stride; int format; uint64_t usage; uint32_t iosurfaceId; };

// ---- HWC1 layer (hardware/hwcomposer.h of N) ------------------------------
struct HwcRect { int32_t left, top, right, bottom; };
struct HwcFRect { float left, top, right, bottom; };
struct HwcRegion { size_t numRects; const HwcRect* rects; };
struct HwcLayer1 {
  int32_t compositionType;
  uint32_t hints;
  uint32_t flags;
  NativeHandle* handle;
  uint32_t transform;
  int32_t blending;
  HwcFRect sourceCropf;
  HwcRect displayFrame;
  HwcRegion visibleRegionScreen;
  int acquireFenceFd;
  int releaseFenceFd;
  uint8_t planeAlpha;
  int _pad;
  uint32_t z;
};
struct HwcDisplayContents1 {
  int retireFenceFd;
  uint64_t dpy, sur;
  size_t numHwLayers;
  HwcLayer1 hwLayers[1];
};

// ---- HWC2 (hwcomposer2.h) --------------------------------------------------
enum Hwc2Error : int32_t { HWC2_ERROR_NONE = 0, HWC2_ERROR_BAD_CONFIG = 5 };
enum Hwc2Composition : int32_t {
  HWC2_COMPOSITION_CLIENT = 0, HWC2_COMPOSITION_DEVICE = 1,
  HWC2_COMPOSITION_SOLID_COLOR = 2, HWC2_COMPOSITION_CURSOR = 3,
  HWC2_COMPOSITION_SIDEBAND = 4
};
using hwc2_function_pointer_t = void (*)(void);
struct Hwc2Display { uint64_t id = 0; };
struct Hwc2Layer { uint64_t id = 0; };

// Present request pushed from the composer to the Metal presenter.
struct PresentRequest {
  int64_t targetNs = 0;
  struct Layer {
    NativeHandle* handle = nullptr;
    HwcFRect sourceCrop{};
    HwcRect displayFrame{};
    uint8_t alpha = 255;
    uint32_t blend = 0;         // 1 premultiplied, 2 coverage
    uint32_t transform = 0;
  };
  std::vector<Layer> layers;
  int32_t compositionType = 0;  // HWC2_COMPOSITION_*
};

// Composer core (portable state; Metal present is in the .mm half).
class HwcComposerCore {
 public:
  static HwcComposerCore& get();
  void configure(int apiLevel, uint32_t fbW, uint32_t fbH);

  // HWC1 entry points (called from the hw_module shim)
  int hwc1Prepare(size_t numDisplays, HwcDisplayContents1** displays);
  int hwc1Set(size_t numDisplays, HwcDisplayContents1** displays);
  int hwc1Blank(int disp, int blank);
  int hwc1Query(int what, int* value);
  int hwc1EventControl(int disp, int event, int enabled);
  int hwc1GetDisplayConfigs(int disp, uint32_t* configs, size_t* numConfigs);
  int hwc1GetDisplayAttributes(int disp, uint32_t config,
                               const uint32_t* attributes, int32_t* values);

  // HWC2 dispatch (function-pointer table indexed by descriptor)
  hwc2_function_pointer_t hwc2GetFunction(int32_t descriptor);
  static int32_t hwc2GetCapabilities(void* device, uint32_t* outCount,
                                     int32_t* outCapabilities);

  // gralloc (Gralloc 0.x used by 7.1; gralloc-minor HAL paths for 8.1/10
  // route through the same allocator)
  int grallocAlloc(uint32_t w, uint32_t h, int format, uint64_t usage,
                   NativeHandle** outHandle, uint32_t* outStride);
  int grallocRegister(NativeHandle* h);
  int grallocLock(NativeHandle* h, uint64_t usage, int l, int t, int w, int hh,
                  void** vaddr);
  int grallocUnlock(NativeHandle* h);
  int grallocRelease(NativeHandle* h);

  // present queue consumed by the Metal layer
  void setPresentSink(std::function<void(const PresentRequest&)> sink) {
    presentSink_ = std::move(sink);
  }
  void vsyncTick();                  // called by the CADisplayLink wrapper
  uint64_t frameCount() const { return frames_; }

 private:
  HwcComposerCore() = default;
  struct LayerState {
    uint64_t layerId = 0;
    NativeHandle* handle = nullptr;
    HwcFRect crop{};
    HwcRect frame{};
    uint8_t alpha = 255;
    uint32_t blend = 1, transform = 0;
    int32_t composition = HWC2_COMPOSITION_CLIENT;
    bool visible = false;
  };
  std::vector<LayerState> layers_;
  std::function<void(const PresentRequest&)> presentSink_;
  int apiLevel_ = 25;
  uint32_t fbW_ = 2160, fbH_ = 1620;
  uint64_t nextLayerId_ = 1;
  std::atomic<uint64_t> frames_{0};
  std::mutex mu_;
};

// Provided by hwc_metal_bridge.mm (Darwin only): attaches the composer to a
// UIKit view; returns an opaque presenter object.
void* hwcMetalAttachToView(void* uiviewController, uint32_t fbW, uint32_t fbH,
                           uint32_t textureCacheBudgetBytes);
void hwcMetalDetach(void* presenter);
void hwcMetalPushTouch(int phase, uint64_t id, double x, double y);
// ANGLE (OpenGL ES -> Metal) loader; call once after boot with the app
// container path. Returns false when the ANGLE dylibs are not bundled.
bool aotdroidAngleLoad(const char* containerPath);

}  // namespace aotdroid
