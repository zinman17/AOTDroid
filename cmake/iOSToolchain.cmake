# ============================================================================
#  AOTDroid-Adaptive-iOS  |  cmake/iOSToolchain.cmake
#  Usage:
#    cmake -S . -B build-ios -G Xcode \
#          -DCMAKE_TOOLCHAIN_FILE=cmake/iOSToolchain.cmake \
#          -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
# ============================================================================
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

# iphoneos SDK from the active Xcode
execute_process(COMMAND xcrun -sdk iphoneos --show-sdk-path
                OUTPUT_VARIABLE IOS_SDK_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_OSX_SYSROOT ${IOS_SDK_PATH})

# Apple Silicon arm64 only; tuned for the minimum supported device (A13).
set(CMAKE_OSX_ARCHITECTURES arm64)
add_compile_options(
  -arch arm64
  -mcpu=apple-a13
  -fobjc-arc
  -fvisibility=hidden
)
add_link_options(
  -arch arm64
  -Wl,-headerpad_max_install_names
)

# Unsigned builds: SideStore/AltStore sign at install time with the user's
# Apple ID. Keep code signing fully disabled for CI artifacts.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "")
