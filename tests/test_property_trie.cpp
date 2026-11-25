// ============================================================================
//  AOTDroid-Adaptive-iOS  |  tests/test_property_trie.cpp
//  Round-trips the property area writer/reader for both layouts:
//  Android 7.1/8.1 flat (v1) and Android 10 serialized trie (v2).
// ============================================================================
#include "runtime/property_area.h"
#include <cassert>
#include <cstdio>

using namespace aotdroid;

static std::vector<std::pair<std::string, std::string>> makeProps(int n) {
  std::vector<std::pair<std::string, std::string>> v;
  for (int i = 0; i < n; ++i) {
    char name[64], value[96];
    snprintf(name, sizeof(name), "ro.test%d.prop%d", i % 7, i);
    snprintf(value, sizeof(value), "value-%d-%s", i, i % 3 == 0 ? "longvaluepaddingxx" : "v");
    v.emplace_back(name, value);
  }
  return v;
}

static void test_flat_layout() {
  auto props = makeProps(64);
  auto area = PropertyAreaBuilder::build(25, props, "u:object_r:default_prop:s0");
  assert(area.main.size() > sizeof(PropAreaHeaderFlat));
  PropAreaHeaderFlat hdr;
  memcpy(&hdr, area.main.data(), sizeof(hdr));
  assert(hdr.magic == kPropAreaMagic);
  assert(hdr.version == kPropAreaVersionFlat);
  assert(hdr.toc_size == 64);
  char out[128];
  for (auto& kv : props) {
    assert(PropertyAreaBuilder::readProp(area.main.data(), area.main.size(), 1,
                                         kv.first.c_str(), out, sizeof(out)));
    assert(std::string(out) == kv.second);
  }
  assert(!PropertyAreaBuilder::readProp(area.main.data(), area.main.size(), 1,
                                        "ro.missing", out, sizeof(out)));
  printf("  flat area (API 25/27) round-trip .... OK (%zu props, %zu bytes)\n",
         props.size(), area.main.size());
}

static void test_trie_layout() {
  auto props = makeProps(64);
  auto area = PropertyAreaBuilder::build(29, props, "u:object_r:default_prop:s0");
  assert(area.main.size() > sizeof(PropAreaHeaderTrie));
  PropAreaHeaderTrie hdr;
  memcpy(&hdr, area.main.data(), sizeof(hdr));
  assert(hdr.magic == kPropAreaMagic);
  assert(hdr.version == kPropAreaVersionTrie);
  char out[128];
  for (auto& kv : props) {
    assert(PropertyAreaBuilder::readProp(area.main.data(), area.main.size(), 2,
                                         kv.first.c_str(), out, sizeof(out)));
    assert(std::string(out) == kv.second);
  }
  // deep nesting: a.b.c.d.e.f
  std::vector<std::pair<std::string, std::string>> deep = {
    {"a.b.c.d.e.f", "deep"}, {"a.b.c.x", "shallow"}, {"a.b", "mid"}};
  auto d2 = PropertyAreaBuilder::build(29, deep, "u:object_r:default_prop:s0");
  assert(PropertyAreaBuilder::readProp(d2.main.data(), d2.main.size(), 2, "a.b.c.d.e.f", out, sizeof(out)));
  assert(std::string(out) == "deep");
  assert(PropertyAreaBuilder::readProp(d2.main.data(), d2.main.size(), 2, "a.b", out, sizeof(out)));
  assert(std::string(out) == "mid");
  assert(!PropertyAreaBuilder::readProp(d2.main.data(), d2.main.size(), 2, "a.b.c.z", out, sizeof(out)));
  // context split files were emitted for ro.* props
  assert(!area.contexts.empty());
  printf("  trie area (API 29) round-trip ....... OK (%zu props, %zu context files)\n",
         props.size(), area.contexts.size());
}

static void test_boot_props() {
  applyBootProperties(25, "arm64-v8a", "iPad12,1", "userdebug");
  assert(PropertyStore::get().get("ro.build.version.sdk") == "25");
  assert(PropertyStore::get().get("ro.treble.enabled") == "false");
  applyBootProperties(29, "arm64-v8a", "iPad13,1", "userdebug");
  assert(PropertyStore::get().get("ro.build.version.sdk") == "29");
  assert(PropertyStore::get().get("ro.treble.enabled") == "true");
  // ro.* immutability
  assert(!PropertyStore::get().set("ro.build.version.sdk", "21"));
  assert(PropertyStore::get().set("persist.sys.test", "1"));
  printf("  boot property matrix ................ OK\n");
}

int main() {
  printf("test_property_trie:\n");
  test_flat_layout();
  test_trie_layout();
  test_boot_props();
  printf("ALL PASS\n");
  return 0;
}
