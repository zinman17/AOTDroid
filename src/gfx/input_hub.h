// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/gfx/input_hub.h   (MODULE 5, input half)
//  Touch translation: UIResponder events -> Android EventHub (/dev/input).
//  Implements the Linux multitouch protocol B (ABS_MT_SLOT +
//  ABS_MT_TRACKING_ID) stream that InputReader expects, plus the evdev
//  ioctls (EVIOCGVERSION / EVIOCGNAME / EVIOCGABS / EVIOCGBIT).
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include "../runtime/vfs.h"

namespace aotdroid {

// Linux event codes
constexpr uint16_t kEV_SYN = 0x00, kEV_KEY = 0x01, kEV_ABS = 0x03;
constexpr uint16_t kSYN_REPORT = 0, kSYN_MT_REPORT = 2;
constexpr uint16_t kBTN_TOUCH = 0x14a;
constexpr uint16_t kABS_MT_SLOT = 0x2f, kABS_MT_TOUCH_MAJOR = 0x30,
                   kABS_MT_WIDTH_MAJOR = 0x32, kABS_MT_POSITION_X = 0x35,
                   kABS_MT_POSITION_Y = 0x36, kABS_MT_TRACKING_ID = 0x39;
constexpr uint16_t kABS_X = 0x00, kABS_Y = 0x01, kABS_PRESSURE = 0x18;

// EVIOCGABS(io) / EVIOCGBIT(ev) / EVIOCGVERSION encodings (function-like)
constexpr uint32_t kEVIOCGVERSION = 0x80044501;
constexpr uint32_t kEVIOCGID = 0x80084502;
#define kEVIOCGNAME(len) \
  ((uint32_t)(0x80000000u | (0x45u << 8) | 0x06u | (((uint32_t)(len) & 0xfffu) << 16)))
#define kEVIOCGBIT(ev, len) \
  ((uint32_t)(0x80000000u | (0x45u << 8) | 0x20u | (((uint32_t)(len) & 0xfffu) << 16) | ((uint32_t)(ev) << 8)))
#define kEVIOCGABS(abs) ((uint32_t)(0x80144540u | (uint32_t)(abs)))
#define kEVIOCSABS(abs) ((uint32_t)(0x401445c0u | (uint32_t)(abs)))

struct LinuxInputEvent {   // 16 bytes on 64-bit (timeval + 3 fields)
  int64_t  tv_sec;
  int64_t  tv_usec;
  uint16_t type, code;
  int32_t  value;
};
struct LinuxInputAbsinfo {
  int32_t value, minimum, maximum, fuzz, flat, resolution;
};

// One emulated touchscreen.
class InputHub {
 public:
  static InputHub& get(int idx);
  void setScreenSize(int w, int h) { w_ = w; h_ = h; }

  // Called by the UIKit layer (hwc_metal_bridge.mm).
  void onTouchBegan(uint64_t id, double x, double y);
  void onTouchMoved(uint64_t id, double x, double y);
  void onTouchEnded(uint64_t id);

  // VNode plumbing (read/ioctl from the syscall bridge).
  long read(VNode* vn, void* buf, size_t n);
  long ioctl(VNode* vn, uint32_t cmd, void* arg);
  uint32_t poll() const { return pending_ ? kEPOLLIN : 0; }

 private:
  InputHub() = default;
  void emit(uint16_t type, uint16_t code, int32_t value);
  void commit();
  int slotOf(uint64_t id);

  std::mutex mu_;
  std::deque<LinuxInputEvent> queue_;
  std::deque<LinuxInputEvent> ready_;    // committed events
  uint64_t tracking_[10] = {0};          // slot -> tracking id (0 = free)
  int nextTracking_ = 1;
  int activeSlot_ = 0;
  bool pending_ = false;
  int w_ = 2160, h_ = 1620;
};

// bridge hooks
int inputHubOpen(int idx);
long inputHubIoctl(VNode* vn, uint32_t cmd, void* arg);

}  // namespace aotdroid
