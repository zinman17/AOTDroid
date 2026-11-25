# ============================================================================
#  AOTDroid-Adaptive-iOS  |  CMakeLists.txt   (MODULE 8)
#  Two build flavors:
#    - HOST  (Linux/macOS x86_64/arm64): portable core + rewriter tool + tests
#      cmake -S . -B build-host -DAOTDROID_HOST=ON
#    - iOS   (Darwin + iPhoneOS SDK): full container app
#      cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#            -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
# ============================================================================
cmake_minimum_required(VERSION 3.22)
# NOTE: do NOT declare OBJCXX/OBJC here -- Linux hosts (ubuntu runners) ship
# g++ without cc1objplus and project(... OBJCXX) then hard-fails at configure
# time. The Apple-only .mm sources are enabled conditionally below.
project(AOTDroid VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AOTDROID_HOST "Build host tooling + tests (no UIKit/Metal)" OFF)
option(AOTDROID_BUILD_TESTS "Build unit tests" ON)
option(AOTDROID_WERROR "Treat warnings as errors" OFF)

# ObjC++ is only required for the Apple (iOS/macOS) app build; host tooling
# and tests are pure C++ and must configure on stock Linux g++/clang.
if(NOT AOTDROID_HOST)
  enable_language(OBJCXX)
endif()

if(AOTDROID_WERROR)
  add_compile_options(-Werror)
endif()

# ---------------------------------------------------------------------------
# Portable core (modules 2/3/4 + composer core + input hub + boot + linker)
# ---------------------------------------------------------------------------
set(AOTDROID_CORE_SOURCES
  src/aot/macho_emit.cpp
  src/aot/aot_rewriter.cpp
  src/runtime/vfs.cpp
  src/runtime/binder_emu.cpp
  src/runtime/property_area.cpp
  src/runtime/domain.cpp
  src/runtime/container_boot.cpp
  src/linker/syscall_bridge.cpp
  src/linker/bionic_linker.cpp
  src/gfx/input_hub.cpp
  src/gfx/hwc_composer_core.cpp
)

if(NOT AOTDROID_HOST)
  # Darwin/iOS-only sources (modules 1/5/7 + app shell)
  list(APPEND AOTDROID_CORE_SOURCES
    src/profiler/hardware_profiler.mm
    src/gfx/hwc_metal_bridge.mm
    src/jetsam/jetsam_guard.mm
    ios/AppShell.mm
  )
endif()

add_library(aotdroid_core STATIC ${AOTDROID_CORE_SOURCES})
target_include_directories(aotdroid_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT AOTDROID_HOST)
  find_library(FOUNDATION Foundation REQUIRED)
  find_library(UIKIT UIKit REQUIRED)
  find_library(METAL Metal REQUIRED)
  find_library(METALKIT MetalKit REQUIRED)
  find_library(QUARTZCORE QuartzCore REQUIRED)
  find_library(COREVIDEO CoreVideo REQUIRED)
  find_library(DISPATCH dispatch REQUIRED)
  target_link_libraries(aotdroid_core PUBLIC
    ${FOUNDATION} ${UIKIT} ${METAL} ${METALKIT} ${QUARTZCORE}
    ${COREVIDEO} ${DISPATCH})
  set_target_properties(aotdroid_core PROPERTIES
    OBJCXX_ARC ON
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0")
endif()

# ---------------------------------------------------------------------------
# Host tool: aotdroid-rewrite (offline pipeline driver)
# ---------------------------------------------------------------------------
add_executable(aotdroid-rewrite src/tools/aotdroid_rewrite_main.cpp)
target_link_libraries(aotdroid-rewrite PRIVATE aotdroid_core)

# ---------------------------------------------------------------------------
# Tests (host + iOS-sim; plain assert binaries, no framework dependency)
# ---------------------------------------------------------------------------
if(AOTDROID_BUILD_TESTS)
  add_executable(test_aot_rewriter tests/test_aot_rewriter.cpp)
  target_link_libraries(test_aot_rewriter PRIVATE aotdroid_core)
  add_executable(test_property_trie tests/test_property_trie.cpp)
  target_link_libraries(test_property_trie PRIVATE aotdroid_core)
  add_executable(test_binder_loopback tests/test_binder_loopback.cpp)
  target_link_libraries(test_binder_loopback PRIVATE aotdroid_core)
  add_test(NAME oat_rewriter COMMAND test_aot_rewriter)
  add_test(NAME property_trie COMMAND test_property_trie)
  add_test(NAME binder_loopback COMMAND test_binder_loopback)
  enable_testing()
endif()

# ---------------------------------------------------------------------------
# iOS app bundle (unsigned -> SideStore re-signs at install)
# ---------------------------------------------------------------------------
if(NOT AOTDROID_HOST AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_executable(AOTDroid MACOSX_BUNDLE ios/AppShell.mm
                 src/profiler/hardware_profiler.mm
                 src/gfx/hwc_metal_bridge.mm
                 src/jetsam/jetsam_guard.mm
                 ${AOTDROID_CORE_SOURCES})
  set_target_properties(AOTDroid PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.aotdroid.container"
    MACOSX_BUNDLE_BUNDLE_NAME "AOTDroid"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    OBJCXX_ARC ON)
  find_library(F_UIKIT UIKit REQUIRED)
  find_library(F_METAL Metal REQUIRED)
  find_library(F_METALKIT MetalKit REQUIRED)
  find_library(F_QUARTZ QuartzCore REQUIRED)
  find_library(F_COREVIDEO CoreVideo REQUIRED)
  target_link_libraries(AOTDroid PRIVATE ${F_UIKIT} ${F_METAL} ${F_METALKIT}
                       ${F_QUARTZ} ${F_COREVIDEO})
endif()
