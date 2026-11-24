# AOTDroid-Adaptive-iOS

An **Ahead-of-Time compiled, unstripped multi-version Android AOSP container**
for **non-jailbroken iPadOS/iOS** devices. No QEMU-style virtualization, no
runtime-JIT dependency: a dual-stage AOT pipeline (offline `dex2oat` +
static ELF→Mach-O binary rewriting) paired with a version-adaptive
Linux→Darwin user-space emulation layer boots real AOSP user spaces —

- **Android 7.1.2 (API 25, OAT 088)** — Tier-1 ultra-stable profile
- **Android 8.1.0 (API 27, OAT 131)** — Tier-1 balanced profile
- **Android 10.0.0 (API 29, OAT 170)** — Tier-2 full profile (APEX + HIDL)

— auto-selected at startup by a Darwin SoC/RAM telemetry probe
(`sysctlbyname("hw.cpufamily" / "hw.memsize")`, `os_proc_available_memory()`).

**Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first** — it explains
the two iOS constraints (code signing, no fork) that shape the whole design,
the relocation funnel, and the verified-vs-unverified matrix.
Memory budgets and the iPad 9 RSS tables live in
[`docs/MEMORY_BUDGET.md`](docs/MEMORY_BUDGET.md).

## Repository layout

```
src/profiler/    module 1  SoC probe + adaptive boot selector (.mm)
src/aot/         module 2  ELF/OAT rewriter: veneers, 16K re-layout,
                           Mach-O emitter + runtime-linker metadata
src/runtime/     modules 3/4 runtime: binder emulator (tri-domain),
                           ashmem, property areas (flat + trie), VFS/fd
                           table, fork-less domain isolator, boot orchestrator
src/linker/      module 4  bionic linker replacement + Darwin syscall bridge
src/gfx/         module 5  HWC1/HWC2 composer core, Metal/IOSurface bridge
                           (.mm), evdev input hub
src/jetsam/      module 7  memory-pressure guard (.mm)
scripts/         module 6  build_multi_aosp_rootfs.sh + host_build.sh
ios/             module 8  AppShell, Info.plist (BootSelector), entitlements
cmake/           module 8  iOS toolchain (arm64, -mcpu=apple-a13)
tests/           unit tests (rewriter / property areas / binder loopback)
.github/         CI: host tests, real-assembler fixture, iOS build, IPA
```

## Quick start (host, no Apple hardware needed)

```bash
./scripts/host_build.sh          # builds aotdroid-rewrite + 3 test binaries, runs them
```

Requirements: `g++ >= 12` (C++20). Everything else is self-contained.

## Building the IPA (macOS with Xcode)

```bash
cmake -S . -B build-ios -G Xcode \
      -DCMAKE_TOOLCHAIN_FILE=cmake/iOSToolchain.cmake \
      -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
cmake --build build-ios --config Release
# or the CI-equivalent one-shot: see .github/workflows/build-ipa.yml (job ios-build)
```

The produced `AOTDroid.app` is **unsigned** — SideStore/AltStore re-sign it
with your Apple ID at install time. The `BootSelector` key in `Info.plist`
(`auto | android-7.1 | android-8.1 | android-10`) plus the in-app launcher
override the automatic SoC-based selection.

## Building rootfs images (needs GSI images)

```bash
scripts/build_multi_aosp_rootfs.sh --profile 7.1 --image 7.1=~/system-7.1.img
scripts/build_multi_aosp_rootfs.sh --image 8.1=~/system-8.1.img
scripts/build_multi_aosp_rootfs.sh --image 10=~/system-10.img
```

Each profile runs that release's own arm64 `dex2oat` over
`BOOTCLASSPATH`/`SYSTEMSERVERCLASSPATH` (via `qemu-aarch64-static` on x86_64),
strips DWARF while keeping symbol tables, freezes the TLS layout, converts
every code image with `aotdroid-rewrite`, and emits
`rootfs-android-<ver>.aotpack` + `manifest.json`.

> Android 7.1.2 has no GSI (pre-Treble): build `aosp_arm64-userdebug` from
> `android-7.1.2_r39` and point `--image` at its `system.img`.

## CI

`.github/workflows/build-ipa.yml` —
`host-tests` (ubuntu+macos), `rewrite-fixture` (rewrites a genuinely
assembled arm64 `.so` and verifies the patched bytes), `ios-build` (compiles
**every** source against the iPhoneOS SDK — the zero-syntax-errors gate — and
packages `AOTDroid-unsigned.ipa`), `macho-link-check` (Apple `ld` accepts the
emitted object), `full-ipa` (manual; rootfs images + conversion).

## Status / honesty

The portable core (modules 2/3/4 + composer core + input + boot) builds and
passes its unit tests on Linux and macOS hosts. The Darwin layers
(modules 1/5/7, app shell) are compiled by the `ios-build` CI job. This is a
working foundation of an unusually deep stack: bringing a full GSI to boot is
milestone work on top of it, tracked honestly in
`docs/ARCHITECTURE.md` §6 (Known limitations).

## License

Apache-2.0 (matching AOSP). Add your `LICENSE` file before distributing
derived rootfs images; AOSP artifacts carry their own notices.
