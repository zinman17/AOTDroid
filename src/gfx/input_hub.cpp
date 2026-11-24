// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/gfx/input_hub.cpp   (MODULE 5)
// ============================================================================
#include "input_hub.h"
#include <sys/time.h>

namespace aotdroid {

namespace {
InputHub* g_hubs[2] = {nullptr, nullptr};
std::mutex g_hubMu;

uint32_t inputPoll(VNode* vn) {
  int idx = (int)vn->cookie;
  return InputHub::get(idx).poll();
}
}  // namespace

InputHub& InputHub::get(int idx) {
  std::lock_guard<std::mutex> lk(g_hubMu);
  if (!g_hubs[idx]) g_hubs[idx] = new InputHub();
  return *g_hubs[idx];
}

void InputHub::emit(uint16_t type, uint16_t code, int32_t value) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  LinuxInputEvent ev{(int64_t)tv.tv_sec, (int64_t)tv.tv_usec, type, code, value};
  queue_.push_back(ev);
}

void InputHub::commit() {
  emit(kEV_SYN, kSYN_REPORT, 0);
  for (auto& e : queue_) ready_.push_back(e);
  queue_.clear();
  pending_ = true;
  VNode::armWakeup(nullptr);
}

int InputHub::slotOf(uint64_t id) {
  for (int i = 0; i < 10; ++i)
    if (tracking_[i] == id) return i;
  for (int i = 0; i < 10; ++i)
    if (tracking_[i] == 0) return i;
  return 0;
}

void InputHub::onTouchBegan(uint64_t id, double x, double y) {
  std::lock_guard<std::mutex> lk(mu_);
  int slot = slotOf(id);
  tracking_[slot] = id;
  emit(kEV_ABS, kABS_MT_SLOT, slot);
  activeSlot_ = slot;
  emit(kEV_ABS, kABS_MT_TRACKING_ID, nextTracking_++);
  emit(kEV_ABS, kABS_MT_POSITION_X, (int32_t)(x * w_));
  emit(kEV_ABS, kABS_MT_POSITION_Y, (int32_t)(y * h_));
  emit(kEV_ABS, kABS_MT_TOUCH_MAJOR, 40);
  emit(kEV_ABS, kABS_MT_WIDTH_MAJOR, 20);
  emit(kEV_KEY, kBTN_TOUCH, 1);
  emit(kEV_ABS, kABS_PRESSURE, 64);
  emit(kEV_ABS, kABS_X, (int32_t)(x * w_));
  emit(kEV_ABS, kABS_Y, (int32_t)(y * h_));
  commit();
}

void InputHub::onTouchMoved(uint64_t id, double x, double y) {
  std::lock_guard<std::mutex> lk(mu_);
  int slot = slotOf(id);
  if (slot != activeSlot_) { emit(kEV_ABS, kABS_MT_SLOT, slot); activeSlot_ = slot; }
  emit(kEV_ABS, kABS_MT_POSITION_X, (int32_t)(x * w_));
  emit(kEV_ABS, kABS_MT_POSITION_Y, (int32_t)(y * h_));
  emit(kEV_ABS, kABS_X, (int32_t)(x * w_));
  emit(kEV_ABS, kABS_Y, (int32_t)(y * h_));
  commit();
}

void InputHub::onTouchEnded(uint64_t id) {
  std::lock_guard<std::mutex> lk(mu_);
  int slot = slotOf(id);
  emit(kEV_ABS, kABS_MT_SLOT, slot);
  emit(kEV_ABS, kABS_MT_TRACKING_ID, -1);
  tracking_[slot] = 0;
  bool any = false;
  for (int i = 0; i < 10; ++i) if (tracking_[i]) any = true;
  if (!any) {
    emit(kEV_KEY, kBTN_TOUCH, 0);
    emit(kEV_ABS, kABS_PRESSURE, 0);
  }
  commit();
}

long InputHub::read(VNode* vn, void* buf, size_t n) {
  std::lock_guard<std::mutex> lk(mu_);
  size_t count = std::min(n / sizeof(LinuxInputEvent), ready_.size());
  if (count == 0) return -EAGAIN;
  LinuxInputEvent* out = (LinuxInputEvent*)buf;
  for (size_t i = 0; i < count; ++i) out[i] = ready_[i];
  ready_.erase(ready_.begin(), ready_.begin() + (long)count);
  if (ready_.empty()) pending_ = false;
  return (long)(count * sizeof(LinuxInputEvent));
}

long InputHub::ioctl(VNode* vn, uint32_t cmd, void* arg) {
  if (cmd == kEVIOCGVERSION) { *(int*)arg = 0x010001; return 0; }
  if (cmd == kEVIOCGID) { memset(arg, 0, 8); return 0; }
  if ((cmd & 0xff) == 0x06) {   // EVIOCGNAME(len)
    size_t len = (cmd >> 16) & 0xfff;
    snprintf((char*)arg, len, "AOTDroid Virtual Touchscreen");
    return (long)strnlen((char*)arg, len);
  }
  if ((cmd & 0xff) == 0x20) {   // EVIOCGBIT(ev, len)
    size_t len = (cmd >> 16) & 0xfff;
    memset(arg, 0, len);
    uint8_t* bits = (uint8_t*)arg;
    int ev = (cmd >> 8) & 0xff;
    if (ev == 0) {                                    // all supported types
      bits[0] = (1 << kEV_SYN) | (1 << kEV_KEY) | (1 << kEV_ABS);
    } else if (ev == kEV_KEY) {
      bits[kBTN_TOUCH / 8] |= 1 << (kBTN_TOUCH % 8);
    } else if (ev == kEV_ABS) {
      bits[kABS_MT_SLOT / 8] |= 1 << (kABS_MT_SLOT % 8);
      bits[kABS_MT_POSITION_X / 8] |= 1 << (kABS_MT_POSITION_X % 8);
      bits[kABS_MT_POSITION_Y / 8] |= 1 << (kABS_MT_POSITION_Y % 8);
      bits[kABS_MT_TRACKING_ID / 8] |= 1 << (kABS_MT_TRACKING_ID % 8);
      bits[kABS_X / 8] |= 1 << (kABS_X % 8);
      bits[kABS_Y / 8] |= 1 << (kABS_Y % 8);
      bits[kABS_PRESSURE / 8] |= 1 << (kABS_PRESSURE % 8);
    }
    return (long)len;
  }
  if ((cmd & 0xffff0000u) == 0x80140000u && (cmd & 0xff00u) == 0x4500u) {
    // EVIOCGABS: fill absinfo for the requested axis
    int abs = (int)(cmd & 0xff);
    if (abs > 0x3f) return -EINVAL;
    LinuxInputAbsinfo* ai = (LinuxInputAbsinfo*)arg;
    memset(ai, 0, sizeof(*ai));
    if (abs == kABS_MT_POSITION_X || abs == kABS_X) {
      ai->minimum = 0; ai->maximum = w_;
    } else if (abs == kABS_MT_POSITION_Y || abs == kABS_Y) {
      ai->minimum = 0; ai->maximum = h_;
    } else if (abs == kABS_MT_SLOT) {
      ai->minimum = 0; ai->maximum = 9;
    } else if (abs == kABS_PRESSURE) {
      ai->minimum = 0; ai->maximum = 255;
    } else {
      ai->minimum = 0; ai->maximum = 255;
    }
    return 0;
  }
  return -ENOTTY;
}

// bridge hooks
int inputHubOpen(int idx) {
  auto vn = std::make_shared<VNode>();
  vn->kind = VNKind::Evdev;
  vn->name = idx == 0 ? "/dev/input/event0" : "/dev/input/event1";
  vn->cookie = (uint64_t)idx;
  vn->pollFn = inputPoll;
  return fds().alloc(vn);
}

long inputHubIoctl(VNode* vn, uint32_t cmd, void* arg) {
  return InputHub::get((int)vn->cookie).ioctl(vn, cmd, arg);
}

}  // namespace aotdroid
