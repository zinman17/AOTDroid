#!/usr/bin/env bash
# ============================================================================
#  AOTDroid-Adaptive-iOS  |  scripts/host_build.sh
#  Host build + test run (no cmake required; mirrors CMakeLists targets).
#  Usage: scripts/host_build.sh [--release]
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:--O0}"
FLAGS="-std=c++20 $MODE -Wall -Wextra -Wno-unused-parameter -Isrc"
SRC="src/aot/macho_emit.cpp src/aot/aot_rewriter.cpp \
     src/runtime/vfs.cpp src/runtime/binder_emu.cpp src/runtime/property_area.cpp \
     src/runtime/domain.cpp src/runtime/container_boot.cpp \
     src/linker/syscall_bridge.cpp src/linker/bionic_linker.cpp \
     src/gfx/input_hub.cpp src/gfx/hwc_composer_core.cpp"
mkdir -p build-host
echo "== building aotdroid-rewrite =="
g++ $FLAGS $SRC src/tools/aotdroid_rewrite_main.cpp -o build-host/aotdroid-rewrite -lpthread
echo "== building tests =="
g++ $FLAGS $SRC tests/test_aot_rewriter.cpp      -o build-host/test_aot_rewriter     -lpthread
g++ $FLAGS $SRC tests/test_property_trie.cpp     -o build-host/test_property_trie    -lpthread
g++ $FLAGS $SRC tests/test_binder_loopback.cpp   -o build-host/test_binder_loopback  -lpthread
echo "== running tests =="
./build-host/test_aot_rewriter
./build-host/test_property_trie
./build-host/test_binder_loopback
echo "== ALL HOST TESTS PASS =="
