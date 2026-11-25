// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/property_area.h   (MODULE 3)
//  Version-aware __system_property_area__ initialization:
//    - Android 7.1 / 8.1: single flat area (magic "PROP", toc of prop_info)
//    - Android 10: /dev/__properties__ multi-file serialized trie layout
//      (context-separated areas + properties_serialized), with per-context
//      files discovered by bionic's __system_properties loader.
//
//  Layout constants mirror bionic <sys/system_properties.h>:
//    PROP_NAME_MAX 32, PROP_VALUE_MAX 92, prop_info serial dirty/len bits.
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"

namespace aotdroid {

constexpr uint32_t kPropNameMax  = 32;
constexpr uint32_t kPropValueMax = 92;
constexpr uint32_t kPropAreaMagic   = 0x504f5250;   // "PROP"
constexpr uint32_t kPropAreaVersionFlat    = 1;     // Android 7.x
constexpr uint32_t kPropAreaVersionTrie    = 2;     // Android 8.0+ serialized

// serial encoding (bionic): dirty flag = bit0, value length = bits 24..31
inline uint32_t prop_serial_make(uint32_t len) {
  return (len << 24) & 0xFF000000u;
}
inline uint32_t prop_serial_len(uint32_t serial) { return (serial >> 24) & 0xff; }
inline bool prop_serial_dirty(uint32_t serial) { return (serial & 1u) != 0; }

#pragma pack(push, 1)
// Android 7.x flat layout
struct PropAreaHeaderFlat {
  uint32_t magic;         // 'PROP'
  uint32_t version;       // 1
  uint32_t reserved[4];
  uint32_t bytes_used;
  uint32_t toc_size;      // number of prop_info records
};
struct PropInfoFlat {
  char name[kPropNameMax];
  uint32_t serial;
  char value[kPropValueMax];
};
static_assert(sizeof(PropInfoFlat) == 128, "PropInfoFlat layout");

// Android 8.0+ / 10 serialized trie layout
struct PropAreaHeaderTrie {
  uint32_t bytes_used;
  uint32_t magic;
  uint32_t version;        // 2
  uint32_t reserved1;
  uint32_t context_offset;
  uint32_t serial;
  uint32_t reserved2;
  uint32_t reserved3;
};
struct PropBt {
  uint32_t serial;
  char name[kPropNameMax];
  uint32_t prop;           // offset of prop_info from area base
  uint32_t left, right, children;   // offsets of prop_bt
};
static_assert(sizeof(PropBt) == 52, "PropBt layout");
struct PropInfoTrie {
  uint32_t serial;
  char value[kPropValueMax];
  char name[kPropNameMax];
};
static_assert(sizeof(PropInfoTrie) == 128, "PropInfoTrie layout");
#pragma pack(pop)

// ---------------------------------------------------------------------------
class PropertyAreaBuilder {
 public:
  // Builds the area for the requested API level and returns the serialized
  // bytes + the per-context split files (Android 10 style) if applicable.
  struct Result {
    std::vector<uint8_t> main;               // properties_serialized / flat area
    std::vector<std::pair<std::string, std::vector<uint8_t>>> contexts;
    uint32_t apiLevel = 0;
  };
  static Result build(uint32_t apiLevel,
                      const std::vector<std::pair<std::string, std::string>>& props,
                      const std::string& defaultContext);

  // Reader used by tests (and by our own /dev/__properties__ synth nodes).
  static bool readProp(const uint8_t* area, size_t areaLen, uint32_t areaVersion,
                       const char* name, char* outValue, size_t outLen);
};

// Runtime-wide property store: the master key/value map backing setprop /
// ro.* defaults injected at boot (ro.build.version.sdk etc).
class PropertyStore {
 public:
  static PropertyStore& get();
  void setDefault(const std::string& k, const std::string& v);
  std::string get(const std::string& k, const std::string& def = "") const;
  bool set(const std::string& k, const std::string& v);
  std::vector<std::pair<std::string, std::string>> all() const;
  size_t count() const;
 private:
  mutable std::mutex mu_;
  std::map<std::string, std::string> props_;
};

// Standard boot properties for a given profile.
void applyBootProperties(uint32_t apiLevel, const char* abi, const char* deviceModel,
                         const char* fingerprintSuffix);

}  // namespace aotdroid
