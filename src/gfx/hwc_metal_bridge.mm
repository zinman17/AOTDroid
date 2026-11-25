// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/gfx/hwc_metal_bridge.mm   (MODULE 5)
//  CoreVideo-backed gralloc (IOSurface-backed CVPixelBuffers) + zero-copy
//  Metal presentation onto CAMetalLayer via CVMetalTextureCache, CADisplayLink
//  vsync, UIResponder touch injection into EventHub, and the ANGLE
//  (GLES -> Metal) loader with tier-aware texture-cache throttling.
//
//  NOTE: <IOSurface/IOSurface.h> and Metal's -newTextureWithDescriptor:
//  iosurface:plane: are NOT public on the iPhoneOS SDK; the public iOS path
//  to the same IOSurface-backed zero-copy buffers is CoreVideo:
//  CVPixelBuffer (kCVPixelBufferIOSurfacePropertiesKey) + CVMetalTextureCache.
// ============================================================================
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <CoreVideo/CVPixelBuffer.h>
#import <CoreVideo/CVMetalTexture.h>
#import <QuartzCore/QuartzCore.h>
#import <dlfcn.h>
#include "hwc_metal_bridge.h"
#include "input_hub.h"

namespace aotdroid {

// ===========================================================================
// gralloc: IOSurface-backed allocation via CVPixelBuffer + CPU lock
// ===========================================================================
namespace {
struct GrallocBuffer {
  CVPixelBufferRef pixelBuffer = nil;
  uint32_t width = 0, height = 0, stride = 0;
  int format = 0;
  uint64_t usage = 0;
};

OSType toCVPixelFormat(int halFormat) {
  switch (halFormat) {
    case kHAL_PIXEL_FORMAT_RGBA_8888: return kCVPixelFormatType_32RGBA;
    // 32RGBX has no public Metal mapping; RGBA is visually identical (alpha
    // is ignored for display) and keeps the zero-copy texture path valid.
    case kHAL_PIXEL_FORMAT_RGBX_8888: return kCVPixelFormatType_32RGBA;
    case kHAL_PIXEL_FORMAT_BGRA_8888: return kCVPixelFormatType_32BGRA;
    default: return kCVPixelFormatType_32BGRA;
  }
}

MTLPixelFormat toMTLPixelFormat(OSType cvf) {
  switch (cvf) {
    case kCVPixelFormatType_32RGBA: return MTLPixelFormatRGBA8Unorm;
    case kCVPixelFormatType_32BGRA: return MTLPixelFormatBGRA8Unorm;
    default: return MTLPixelFormatBGRA8Unorm;
  }
}

std::atomic<uint32_t> g_nextSurfaceId{1};   // our own buffer registry id

// surfaceId -> live buffer registry
std::unordered_map<uint32_t, GrallocBuffer*>& byId() {
  static std::unordered_map<uint32_t, GrallocBuffer*> m;
  return m;
}
}  // namespace

void* grallocDarwinAlloc(uint32_t w, uint32_t h, int format, uint64_t usage,
                         uint32_t* stride) {
  OSType cvf = toCVPixelFormat(format);
  CVPixelBufferRef pb = nil;
  NSDictionary* attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
  if (CVPixelBufferCreate(nil, w, h, cvf,
                          (__bridge CFDictionaryRef)attrs, &pb) != kCVReturnSuccess)
    return nullptr;
  // CVPixelBufferCreate returns a buffer with +1 retain; the registry owns it.
  auto* b = new GrallocBuffer();
  b->pixelBuffer = pb;
  b->width = w; b->height = h;
  b->stride = (uint32_t)(CVPixelBufferGetBytesPerRow(pb) / 4);
  b->format = format;
  b->usage = usage;
  if (stride) *stride = b->stride;
  auto* nh = new NativeHandle();
  nh->fd = -1;
  nh->magic = 0x5a4f5452;   // 'ZOTR'
  nh->version = 1;
  nh->width = w; nh->height = h;
  nh->stride = b->stride;
  nh->format = format;
  nh->usage = usage;
  nh->iosurfaceId = g_nextSurfaceId.fetch_add(1);
  byId()[nh->iosurfaceId] = b;
  return nh;
}

void* grallocDarwinLock(NativeHandle* h) {
  auto& m = byId();
  auto it = m.find(h->iosurfaceId);
  if (it == m.end()) return nullptr;
  CVPixelBufferLockBaseAddress(it->second->pixelBuffer, 0);
  return CVPixelBufferGetBaseAddress(it->second->pixelBuffer);
}
void grallocDarwinUnlock(NativeHandle* h) {
  auto& m = byId();
  auto it = m.find(h->iosurfaceId);
  if (it != m.end()) CVPixelBufferUnlockBaseAddress(it->second->pixelBuffer, 0);
}
void grallocDarwinFree(NativeHandle* h) {
  auto& m = byId();
  auto it = m.find(h->iosurfaceId);
  if (it != m.end()) {
    CVPixelBufferRelease(it->second->pixelBuffer);
    delete it->second;
    m.erase(it);
  }
  delete h;
}

// ===========================================================================
// Metal presenter
// ===========================================================================
namespace {
id<MTLDevice> g_device = nil;
id<MTLCommandQueue> g_queue = nil;
CAMetalLayer* g_layer = nil;
CADisplayLink* g_displayLink = nil;
CVMetalTextureCacheRef g_cvTexCache = nil;
// texture cache with tier-aware byte budget (LRU eviction)
struct CacheEnt { id<MTLTexture> tex; CVMetalTextureRef cvTex; uint64_t lastUse; size_t bytes; };
std::unordered_map<uint32_t, CacheEnt> g_texCache;
uint64_t g_cacheBudget = 96 * 1024 * 1024;   // 96 MB default (Tier 1)
uint64_t g_cacheUsed = 0;
uint64_t g_frame = 0;

id<MTLTexture> textureForBuffer(CVPixelBufferRef pb, uint32_t key) {
  auto it = g_texCache.find(key);
  if (it != g_texCache.end()) {
    it->second.lastUse = g_frame;
    return it->second.tex;
  }
  if (!g_cvTexCache || !pb) return nil;
  size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
  size_t rowBytes = CVPixelBufferGetBytesPerRow(pb);
  CVMetalTextureRef cvTex = nil;
  CVReturn cr = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault, g_cvTexCache, pb, nil,
      toMTLPixelFormat(CVPixelBufferGetPixelFormatType(pb)),
      (size_t)w, (size_t)h, 0, &cvTex);
  if (cr != kCVReturnSuccess || !cvTex) return nil;
  id<MTLTexture> tex = CVMetalTextureGetTexture(cvTex);
  if (!tex) { CFRelease(cvTex); return nil; }
  size_t bytes = rowBytes * h;
  // LRU eviction to stay inside the tier budget
  while (g_cacheUsed + bytes > g_cacheBudget && !g_texCache.empty()) {
    auto oldest = g_texCache.begin();
    for (auto it2 = g_texCache.begin(); it2 != g_texCache.end(); ++it2)
      if (it2->second.lastUse < oldest->second.lastUse) oldest = it2;
    g_cacheUsed -= oldest->second.bytes;
    if (oldest->second.cvTex) CFRelease(oldest->second.cvTex);
    g_texCache.erase(oldest);
  }
  g_texCache[key] = {tex, cvTex, g_frame, bytes};
  g_cacheUsed += bytes;
  return tex;
}
}  // namespace
}  // namespace aotdroid   (ObjC classes below must live at global scope)

@interface AOTMetalView : UIView
@end
@implementation AOTMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (void)touchesBegan:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {
  [super touchesBegan:t withEvent:e];
  for (UITouch* touch in t) {
    CGPoint p = [touch locationInView:self];
    aotdroid::hwcMetalPushTouch(0, (uint64_t)(uintptr_t)touch,
                                p.x / self.bounds.size.width,
                                p.y / self.bounds.size.height);
  }
}
- (void)touchesMoved:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {
  [super touchesMoved:t withEvent:e];
  for (UITouch* touch in t) {
    CGPoint p = [touch locationInView:self];
    aotdroid::hwcMetalPushTouch(1, (uint64_t)(uintptr_t)touch,
                                p.x / self.bounds.size.width,
                                p.y / self.bounds.size.height);
  }
}
- (void)touchesEnded:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {
  [super touchesEnded:t withEvent:e];
  for (UITouch* touch in t) {
    CGPoint p = [touch locationInView:self];
    aotdroid::hwcMetalPushTouch(2, (uint64_t)(uintptr_t)touch,
                                p.x / self.bounds.size.width,
                                p.y / self.bounds.size.height);
  }
}
- (void)touchesCancelled:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {
  [super touchesCancelled:t withEvent:e];
  for (UITouch* touch in t)
    aotdroid::hwcMetalPushTouch(2, (uint64_t)(uintptr_t)touch, 0, 0);
}
@end

// ---------------------------------------------------------------------------
// CADisplayLink target (vsync ticks for the composer)
// ---------------------------------------------------------------------------
@interface AOTDisplayLinkTarget : NSObject
@end
@implementation AOTDisplayLinkTarget
- (void)tick:(CADisplayLink*)dl {
  aotdroid::HwcComposerCore::get().vsyncTick();
  (void)dl;
}
@end

namespace aotdroid {
namespace {
void presentRequest(const PresentRequest& req) {
  if (!g_layer || !g_queue) return;
  id<CAMetalDrawable> drawable = [g_layer nextDrawable];
  if (!drawable) return;
  id<MTLCommandBuffer> cb = [g_queue commandBuffer];
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = drawable.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionClear;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
  for (const auto& l : req.layers) {
    auto it = byId().find(l.handle ? l.handle->iosurfaceId : 0);
    if (it == byId().end()) continue;
    id<MTLTexture> tex = textureForBuffer(it->second->pixelBuffer,
                                          l.handle->iosurfaceId);
    if (!tex) continue;
    // blit-based layer placement (full pipeline shaders live in the ANGLE path)
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
              sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake((NSUInteger)l.sourceCrop.left,
                                        (NSUInteger)l.sourceCrop.top, 0)
               sourceSize:MTLSizeMake((NSUInteger)(l.sourceCrop.right - l.sourceCrop.left),
                                      (NSUInteger)(l.sourceCrop.bottom - l.sourceCrop.top), 1)
                toTexture:drawable.texture
         destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake((NSUInteger)l.displayFrame.left,
                                        (NSUInteger)l.displayFrame.top, 0)];
    [blit endEncoding];
  }
  [enc endEncoding];
  [cb presentDrawable:drawable];
  [cb commit];
  g_frame++;
}
}  // namespace

void* hwcMetalAttachToView(void* uivc, uint32_t fbW, uint32_t fbH,
                           uint32_t textureCacheBudgetBytes) {
  UIViewController* vc = (__bridge UIViewController*)uivc;
  g_device = MTLCreateSystemDefaultDevice();
  if (!g_device) return nullptr;
  g_queue = [g_device newCommandQueue];
  AOTMetalView* view = [[AOTMetalView alloc] initWithFrame:vc.view.bounds];
  g_layer = (CAMetalLayer*)view.layer;
  g_layer.device = g_device;
  g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  g_layer.drawableSize = CGSizeMake(fbW, fbH);
  g_layer.framebufferOnly = NO;   // gralloc buffers are shared CPU/GPU
  [vc.view addSubview:view];
  g_cacheBudget = textureCacheBudgetBytes ? textureCacheBudgetBytes
                                          : 96 * 1024 * 1024;
  // zero-copy texture path: CoreVideo Metal texture cache over the
  // IOSurface-backed pixel buffers (public API on iOS)
  if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, g_device, nil,
                                &g_cvTexCache) != kCVReturnSuccess) {
    g_cvTexCache = nil;
    AOTD_LOGW("hwc", "CVMetalTextureCacheCreate failed; presentation disabled");
  }
  HwcComposerCore::get().setPresentSink(&presentRequest);
  InputHub::get(0).setScreenSize((int)fbW, (int)fbH);

  // vsync ticks drive HWC vsync callbacks + frame stats
  g_displayLink = [CADisplayLink displayLinkWithTarget:[AOTDisplayLinkTarget new]
                                               selector:@selector(tick:)];
  [g_displayLink addToRunLoop:[NSRunLoop mainRunLoop]
                      forMode:NSRunLoopCommonModes];
  return (__bridge void*)view;
}

void hwcMetalDetach(void* presenter) {
  [g_displayLink invalidate];
  g_displayLink = nil;
  if (presenter) {
    UIView* v = (__bridge UIView*)presenter;   // non-owning bridge
    [v removeFromSuperview];
  }
}

void hwcMetalPushTouch(int phase, uint64_t id, double x, double y) {
  auto& hub = InputHub::get(0);
  switch (phase) {
    case 0: hub.onTouchBegan(id, x, y); break;
    case 1: hub.onTouchMoved(id, x, y); break;
    default: hub.onTouchEnded(id); break;
  }
}

// ===========================================================================
// ANGLE (OpenGL ES 3.2 -> Metal) loader
// ===========================================================================
namespace {
void* g_angleEGL = nullptr;
void* g_angleGLESv2 = nullptr;
bool g_angleTried = false;
}

bool aotdroidAngleLoad(const char* containerPath) {
  if (g_angleTried) return g_angleEGL != nullptr;
  g_angleTried = true;
  std::string base = containerPath ? containerPath : ".";
  g_angleEGL = dlopen((base + "/Frameworks/libEGL.dylib").c_str(),
                      RTLD_NOW | RTLD_LOCAL);
  if (!g_angleEGL) {
    AOTD_LOGE("angle", "libEGL.dylib: %s", dlerror());
    return false;
  }
  g_angleGLESv2 = dlopen((base + "/Frameworks/libGLESv2.dylib").c_str(),
                         RTLD_NOW | RTLD_LOCAL);
  if (!g_angleGLESv2) {
    AOTD_LOGE("angle", "libGLESv2.dylib: %s", dlerror());
    return false;
  }
  // SurfaceFlinger + Skia resolve eglGetProcAddress through us on the
  // emulated side; the Metal backend is selected via the ANGLE_platform_angle
  // angle_backing_renderer = "metal" platform type (default on iOS).
  AOTD_LOGI("angle", "ANGLE Metal backend loaded (EGL + GLESv2)");
  return true;
}

}  // namespace aotdroid
