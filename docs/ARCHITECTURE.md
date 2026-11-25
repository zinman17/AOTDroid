# AOTDroid-Adaptive-iOS — Architecture Specification

**AOT-compiled, unstripped multi-version Android AOSP container for non-jailbroken iPadOS/iOS.**
Android 7.1.2 (API 25) · Android 8.1 (API 27) · Android 10 (API 29), auto-selected by SoC tier.

---

## 1. Execution model — and the two hard constraints that shape it

The system runs an **unstripped AOSP user space** (`init`, `servicemanager`,
`zygote64`, `system_server`, `SurfaceFlinger`, Binder IPC) on stock iPadOS
with:

1. **No hardware virtualization** (no QEMU/UTM-style guest).
2. **No runtime JIT as the primary engine** — execution is Ahead-of-Time
   compiled code, with interpretation (ART interpreter, not JIT) as the
   fallback for late-loaded DEX.

Two iOS platform realities drive every design decision:

### 1.1 Code signing kills on-device code generation

iOS will not map `PROT_EXEC` pages that are not covered by the app's code
signature. `mmap(PROT_EXEC)` of a data file (a downloaded `.oat`, a patched
`.so`) is terminated by the kernel. Therefore **every byte of executable code
must exist inside the signed app bundle at install time**:

* The **offline stage** of the dual-stage pipeline runs entirely in CI / on a
  build host (module 6 script + module 2 rewriter): `dex2oat` with each
  release's *own* binary → ELF `.oat`/`.odex`/`.so` → **static binary
  rewriting** (syscall + TLS veneers) → **ELF→Mach-O conversion** → the
  converted objects are **linked by ld64 into the app binary**, so their code
  pages are signed with the app.
* The **install-time stage** on device is verification and state setup only:
  profile selection (module 1), rootfs pack mount, property area
  initialization, binder domain setup. It never writes executable bytes.
* With the optional `com.apple.security.cs.allow-jit` entitlement (SideStore
  per-app toggle), an *optional upgrade path* opens: on-device AOT of
  side-loaded app DEX via `MAP_JIT`. The architecture does not depend on it.

### 1.2 fork() is prohibited

iOS terminates the process on `fork()`. The AOSP boot depends on
`fork()/execve()` (init starting services; zygote specializing app processes)
and `clone()` (bionic pthreads). Module 4's **Single-Process Multi-Domain
Isolator** (`src/runtime/domain.*`) replaces the process abstraction:

* Every Android "process" is a **ProcessDomain**: pid, ppid, thread group,
  cwd, signal mask, binder identity, exit status.
* `clone(CLONE_VM|CLONE_THREAD|CLONE_SIGHAND)` → pthreads inside the domain.
* Fork-style clone from **audited callers** (`init`, `app_process`, `zygote`)
  → new domain; on device builds the child continues on a **relocated copy of
  the caller stack** (register snapshot + frame-pointer chain relocation);
  unaudited callers are refused with `-EPERM`.
* `execve(path)` → the current thread re-homes into a **pre-linked image from
  the exec registry** (fresh stack, argv/envp), i.e. the converted AOSP
  binaries linked at build time.
* **Zygote COW without fork**: the zygote preloaded heap is
  `mprotect(PROT_READ)`-guarded; a write fault inside the guard triggers
  page-granular copy-on-write in the SIGSEGV handler (`DomainTable::cowAddRange`
  / `cowHandleWriteFault`). Dirty pages cost exactly what they would under a
  real fork's COW.

## 2. Dual-stage AOT pipeline

```
                     OFFLINE (CI / build host)                       DEVICE (install/boot)
 ┌───────────────────────────────────────────────────────┐   ┌──────────────────────────────────┐
 │ GSI system.img (7.1.2 / 8.1 / 10, arm64)              │   │ hardware_profiler.mm             │
 │   │ debugfs unpack (no root)                           │   │   sysctl hw.cpufamily/memsize/   │
 │   ▼                                                   │   │   hw.machine, os_proc_available  │
 │ dex2oat (079/088 · 124/131 · 170)                     │   │   ▼                              │
 │   BOOTCLASSPATH + SYSTEMSERVERCLASSPATH                │   │ decision matrix → BootProfile    │
 │   → boot.art / boot.oat / services.odex  (arm64)       │   │   rootfs-<ver>.aotpack           │
 │   │                                                   │   │   -Xmx/-Xms, fb scale            │
 │   ▼                                                   │   │   ▼                              │
 │ aot_rewriter (module 2)                               │   │ container_boot.cpp               │
 │   • scan .text: svc #0 / mrs|msr tpidr_el0            │   │   pack mount, properties          │
 │   • veneers → Darwin bridge (see §3)                  │   │   binder contexts (3 domains)     │
 │   • 16 KiB PT_LOAD re-layout                          │   │   init domain (pid 1)             │
 │   • TLS relocs frozen via link-set layout             │   │   ▼                              │
 │   • ELF → Mach-O object (__aotgot/__aotmeta)          │   │ jetsam_guard (module 7) armed     │
 │   ▼                                                   │   │   ▼                              │
 │ ld64 links converted objects into the signed app      │   │ AOSP user space boots            │
 │ llvm-strip --strip-debug (symtabs kept = unstripped)  │   │   init → servicemanager →         │
 │   ▼                                                   │   │   zygote64 → system_server →      │
 │ rootfs-<ver>.aotpack + manifest.json (sha256)         │   │   SurfaceFlinger                  │
 └───────────────────────────────────────────────────────┘   └──────────────────────────────────┘
```

### 2.0 On-device GSI import (src/rootfs/)

The launcher's **Add RootFS** flow replaces the offline-only pack story for
end users: a GSI `.zip` picked via `UIDocumentPickerViewController` is
converted entirely on-device by `RootfsManager` —

```
zip_reader (EOCD/ZIP64; stored = random-access pread, deflate = stream)
   → sparse_image (Android sparse chunk map: raw / fill / dont-care)
   → ext4_reader (extents depth 0..n, direct-block fallback, fast symlinks,
                   64-bit BGD, metadata_csum images)
   → pack_writer  (rootfs-<tag>.aotpack: sorted entries, mmap-served,
                   16 KiB-aligned exec blobs, symlinks stored as targets)
```

Pack entry flags: bit0 = exec (16 KiB aligned), bit1 = dir,
bit2 = symlink (blob holds the target string; `PackVFS::lookup` follows
chains lexically, `readlinkat` is served from the raw entry). The profile
tag comes from `ro.build.version.sdk` (≤25 → `android-7.1`, ≤28 →
`android-8.1`, else `android-10`), and `containerBoot` aligns the boot
profile with whatever pack is installed.


### 2.1 OAT version matrix

| AOSP release | API | OAT header version | binder protocol | layout |
|---|---|---|---|---|
| 7.1.2 | 25 | **079 / 088** | 6 | single `/dev/binder`, flat property area (v1) |
| 8.1.0 | 27 | **124 / 131** | 7 | Treble-capable; vndbinder/hwservicemanager optional (Tier-1 profile disables the split to save RSS) |
| 10.0.0 | 29 | **170** | 8 | `/apex/com.android.runtime`, HIDL HALs, serialized trie property area (v2) |

### 2.2 The relocation funnel (module 2 → module 4)

Every ELF relocation in the AOSP images is routed one of three ways:

1. **Internal** (symbol defined in the same image) → translated to a Mach-O
   relocation; **ld64** resolves it at app link time.
2. **External data/function** (liblog from libc, etc.) → an 8/16-byte **slot**
   in `__AOTDATA,__aotgot`; the instruction sequence is re-pointed at the slot
   (`ADRP+ADD` becomes `ADRP+LDR` through the slot — same instruction width);
   the runtime linker (`bionic_linker.cpp`) fills slots before any init code
   runs (BIND_NOW semantics).
3. **Runtime data fixups** (`GLOB_DAT`, `JUMP_SLOT`, `TPREL64`, `TLSDESC`) →
   serialized records in `__AOTDATA,__aotmeta`; the runtime applies them at
   addresses obtained from `getsectiondata()`.

TLS is handled by **freezing the static TLS layout at build time** (module 6
assigns each DSO's `PT_TLS` block a fixed offset in the emulated per-thread
TP area, emitted as `tls-<ver>.tsv`). TLSLE/TLSIE immediates are patched into
the instructions at emit time; TLSDESC descriptors are resolved to a signed
resolver stub with the frozen offset as its argument.

### 2.3 Syscall / TLS veneers

Pattern scan over executable segments (verified encodings, see §7):

| pattern | match | action |
|---|---|---|
| `svc #0` | `0xD4000001` | site → `b` to a veneer: `bl aotdroid_syscall_bridge` then `b site+4` |
| `mrs xN, tpidr_el0` | `(w & 0xFFFFFFE0) == 0xD53BD040` | veneer calls `aotdroid_get_android_tls_c`, result into xN, full frame save/restore |
| `mrs xN, tpidrro_el0` | `…0xD53BD060` | same (read-only TLS view = same emulated TP) |
| `msr tpidr_el0, xN` | `…0xD51BD040` | veneer calls `aotdroid_set_android_tls_c(xN)` |

The asm bridge (`syscall_bridge.cpp`, aarch64+Apple only) preserves
`x1..x17, x30` across the call — mirroring the Linux arm64 syscall contract
(kernel restores everything except x0). Out-of-range sites (> ±128 MiB,
possible on `boot.oat`-class images) fall back to `brk #idx` trampoline traps
decoded by the in-process Mach exception server.

**4 KB → 16 KB page alignment**: PT_LOADs are re-laid out so
`p_offset ≡ p_vaddr (mod 16384)` with page-aligned offsets, and RX/RW
segments never share a 16 KiB page. The section header table and dynamic
entries are fixpoint-adjusted. The iOS build passes
`-mcpu=apple-a13` and links with ld64 defaults that keep 16 KiB segment
alignment.

## 2.4 `/proc` & device surface

`/proc/self/maps` is synthesized from the mmap registry (ART reads it);
`/proc/cpuinfo` synthesizes a 6-core big.LITTLE arm64; `/sys/devices/system/
cpu/present` reports `0-5`; `/dev/binder|hwbinder|vndbinder|ashmem|urandom|
input/event0` are special VNodes in the emulated fd table. `AT_SYSINFO_EHDR`
is reported empty so bionic's vDSO probe falls back to raw syscalls — which
the bridge handles — instead of needing a code-emitting vDSO.

## 3. Module map

| # | Module | Files | Notes |
|---|---|---|---|
| 1 | SoC & memory probe + boot selector | `src/profiler/hardware_profiler.{h,mm}` | decision matrix, `-Xmx` tuning, fb scaling; RSS budgets in `docs/MEMORY_BUDGET.md` |
| 2 | Multi-version AOT rewriter | `src/aot/{elf_oat_abi.h, aot_rewriter.*, macho_emit.*}` | OAT 079→170, veneers, 16 K layout, Mach-O emitter |
| 3 | Binder + ashmem + properties | `src/runtime/{binder_emu.*, property_area.*}` | tri-domain, flat & trie layouts, `ro.build.version.sdk` = 25/27/29 |
| 4 | Bionic linker + syscall bridge | `src/linker/{bionic_linker.*, syscall_bridge.*}`, `src/runtime/{vfs.*, domain.*, container_boot.*}` | futex→ulock, epoll→kqueue, eventfd/timerfd, openat VFS, clone/execve isolator |
| 5 | HWC1/HWC2 + Metal + input | `src/gfx/{hwc_metal_bridge.*, hwc_composer_core.cpp, input_hub.*}` | IOSurface zero-copy, CAMetalLayer, ANGLE, protocol-B touch |
| 6 | Rootfs builder | `scripts/build_multi_aosp_rootfs.sh` | per-version dex2oat, llvm-strip, TLS freeze, pack |
| 7 | Jetsam guard | `src/jetsam/jetsam_guard.{h,mm}` + `aotdroid_memory_pressure_notify` | WARN/CRITICAL → trim/purge/madvise + 220 MB watchdog |
| 8 | Build & packaging | `CMakeLists.txt`, `cmake/iOSToolchain.cmake`, `ios/*`, workflow | unsigned IPA for SideStore/AltStore |

## 4. Graphics & input path

* **Gralloc** buffers are `IOSurface`s (CVPixelBuffer-backed for CPU locks);
  the gralloc handle carries the `IOSurfaceID`.
* **HWC1** (7.1): module version 1.5 `hw_module_t`, `prepare/set/eventControl/
  query/getDisplayConfigs/getDisplayAttributes`.
* **HWC2** (8.1/10): function-pointer dispatch (`hwc2_device_t` style) +
  registration paths for the hwbinder-hosted composer; both funnel into the
  single `HwcComposerCore` state machine.
* **Present**: composer layers blit (`MTLBlitCommandEncoder`) from their
  IOSurface-backed `MTLTexture`s onto `CAMetalLayer.nextDrawable`; vsync is
  `CADisplayLink`. A 96 MiB LRU texture cache (tier-tuned) throttles IOSurface
  texture residency on 3 GB devices.
* **GLES/EGL**: ANGLE (OpenGL ES → Metal) loaded from bundled
  `libEGL.dylib`/`libGLESv2.dylib` (`aotdroidAngleLoad`).
* **Touch**: `UIResponder` → `InputHub` → Linux multitouch protocol B
  (`ABS_MT_SLOT`, `ABS_MT_TRACKING_ID`, `BTN_TOUCH`, `SYN_REPORT`) into
  `/dev/input/event0`, with `EVIOCGABS`/`EVIOCGBIT` ioctls — so 7.1's
  `EventHub`/`InputReader` and WindowManagerService dispatch unchanged.

## 5. What is verified, and how

| Claim | Verification |
|---|---|
| ARM64 encoders | `tests/test_aot_rewriter.cpp` pins every constant to bytes produced by an independent assembler; CI `rewrite-fixture` re-checks against real `aarch64-linux-gnu-as` output |
| Reloc numbers | `static_assert` against glibc `<elf.h>` (host) |
| 16 K re-layout invariants | test + CI python checker on a real assembled `.so` |
| Mach-O emission | CI `macho-link-check` feeds the object to Apple `ld` |
| Binder semantics | `test_binder_loopback.cpp`: 3-domain servicemanager flow, handle translation, tri-domain isolation, protocol versions |
| Property areas | `test_property_trie.cpp`: 64-prop round-trips for flat and trie layouts |
| Pack format | packer (python) ↔ `PackVFS` (C++) round-trip |
| No syntax errors | CI `ios-build` compiles every `.cpp/.mm` against the iPhoneOS SDK with `-Wall -Wextra` |
| CI cross-checks vs real GSIs | binder/BINDER_* encodings, bionic `prop_info` sizes, HWC2 function order and `composer@2.1` transaction codes are extracted from the pinned GSI headers in the rootfs job and diffed against the tables here |

## 6. Known limitations (honest list)

1. **Stack-copy pseudo-fork** is safe only for the audited clone windows
   (init, app_process/zygote); a non-audited fork returns `-EPERM` loudly.
2. `mremap` is copy-relocate; moving a region with held futexes is not
   supported (ART heap growth is satisfied by our allocator).
3. HIDL parcel hand-rolls `composer@2.1` transactions; the CI cross-check
   diffs codes against the pinned `.hal`. Other HIDL HALs ride the same
   pattern as they are brought up.
4. JIT-less app execution is interpreter-speed for side-loaded DEX until the
   optional MAP_JIT AOT path runs (entitled devices only).
5. The binder emulator serializes node tables under one mutex per domain —
   sufficient for system_server-class traffic at container scale; sharding is
   the scaling path.
