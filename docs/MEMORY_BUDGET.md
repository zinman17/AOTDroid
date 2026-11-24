# Comparative RSS Memory Budget — iPad 9 (A13, 3 GB RAM)

**Methodology.** These are *engineering budgets* (targets + allocation
ceilings), not measurements: they encode the constraints the decision matrix
in `hardware_profiler.mm` enforces and the numbers the rootfs build tunes
`dex2oat`/ART flags toward. Columns show per-"process" resident set, split
into **shared clean** (mapped file pages counted once for the whole container:
`boot.art`/`boot.oat` images, framework jars, libc) and **private dirty**
(heap, stacks, JIT-less interpreter structures). Final validation is the CI
profiling milestone: `footprint` on a development-signed build.

**Jetsam context (iPad 9, iPadOS 15+):** per-app kill threshold ≈ **1.8 GB**
footprint on 3 GB devices. The guard rail in `selectOptimalAndroidProfile()`
requires `rssBudget + 256 MB ≤ os_proc_available_memory()` at boot, or the
profile auto-downgrades (manual overrides keep their choice with aggressive
`-Xmx` clamping + `madvise` eviction).

---

## Android 7.1.2 (API 25) — default Tier-1 profile

| Component | Shared clean (MB) | Private dirty (MB) | RSS total (MB) | Notes |
|---|---:|---:|---:|---|
| `init` | 1 | 3 | 4 | rc parser + property service thread |
| `servicemanager` | 2 | 1 | 3 | single binder domain |
| `logd` | 4 | 8 | 12 | ring buffers capped 256 KB each |
| `zygote64` (preloaded) | 0 | 148 | 148 | N preload list ≈ 10k classes; 96 MB zygote heap (COW-guarded), 52 MB runtime/stacks/metadata |
| `system_server` (AMS/PMS/WMS/…) | 0 | 322 | 322 | `-Xmx384m`, ~118 services on N; PSS after settle |
| `SurfaceFlinger` | 6 | 58 | 64 | HWC1, 1620×1215 fb (0.75×), triple buffer 7.9 MB/buf |
| `boot.art` + `boot.oat` (shared, counted once) | 96 | — | 96 | `--compiler-filter=speed`, OAT 088 |
| framework jars / libc.so / libart (shared) | 42 | — | 42 | symtabs kept, DWARF stripped |
| GPU/IOSurface texture cache | — | 48 | 48 | 96 MB budget, LRU, Tier-1 throttle |
| Dalvik heap (apps) | — | 64 | 64 | `-Xmx192m` default app |
| **TOTAL** | **191** | **716** | **≈ 650–850** | **within the 850 MB budget** |

## Android 8.1.0 (API 27) — Tier-1 "Balanced" profile

| Component | Shared clean (MB) | Private dirty (MB) | RSS total (MB) | Notes |
|---|---:|---:|---:|---|
| `init` | 1 | 4 | 5 | |
| `servicemanager` | 2 | 1 | 3 | |
| `logd` | 5 | 10 | 15 | |
| `zygote64` | 0 | 192 | 192 | O preload list grown; 128 MB zygote heap |
| `system_server` | 0 | 438 | 438 | O service count +; `-Xmx448m` |
| `SurfaceFlinger` | 7 | 74 | 81 | HWC2 path; same 0.75× fb |
| `boot.art` + `boot.oat` | 118 | — | 118 | OAT 131, speed filter |
| shared libs/jars | 52 | — | 52 | |
| GPU/IOSurface cache | — | 56 | 56 | |
| Dalvik heap (apps) | — | 96 | 96 | |
| **TOTAL** | **185** | **869** | **≈ 850–1,100** | **within the 1,100 MB budget; vndbinder/hwservicemanager split disabled on Tier 1 (saves ≈ 60–90 MB vs full Treble)** |

## Android 10.0.0 (API 29) — Tier-2 profile (A14/M1+, 4–16 GB)

| Component | Shared clean (MB) | Private dirty (MB) | RSS total (MB) | Notes |
|---|---:|---:|---:|---|
| `init` | 1 | 5 | 6 | |
| `servicemanager` | 2 | 2 | 4 | tri-domain aware |
| `hwservicemanager` + `vndservicemanager` | 3 | 6 | 9 | full Treble split ON |
| `logd` (+ `statsd` thread) | 6 | 14 | 20 | |
| `zygote64` | 0 | 236 | 236 | Q preloads + app-zygote support |
| `system_server` | 0 | 612 | 612 | `-Xmx640m`; Q adds gfxstats, NN, incremental |
| `SurfaceFlinger` | 9 | 88 | 97 | HWC2 + composer@2.1 over hwbinder |
| HIDL HAL processes (in-process domains) | 8 | 34 | 42 | allocator@2.0, mapper@2.0, composer@2.1, sensors |
| `boot.art` + `boot.oat` (apex) | 158 | — | 158 | OAT 170; `com.android.runtime` APEX |
| shared libs/jars (incl. APEX libs) | 64 | — | 64 | |
| GPU/IOSurface cache | — | 96 | 96 | full-res fb on 4 GB+ |
| Dalvik heap (apps) | — | 160 | 160 | `-Xmx256m` default app |
| **TOTAL** | **251** | **1,287** | **≈ 1,400–1,900** | **within the 1,900 MB budget — Tier-2 devices only** |

---

## Decision matrix (what `selectOptimalAndroidProfile()` enforces)

| SoC / RAM | Automatic selection | Why |
|---|---|---|
| A13 and older, 3 GB (iPad12,1/2, iPhone 11, SE2) | **Android 7.1.2**; Android 8.1 selectable when `os_proc_available_memory() ≥ 1.35 GB` | No APEX modules, no Treble HAL splits; full `system_server` + SF + zygote64 comfortably under the 1.8 GB Jetsam threshold |
| A13 class, 4 GB | Android 8.1 | headroom covers the O-profile growth |
| A14 / A15 / M1+, 4–16 GB (iPad Air 4/5, iPad 10, iPad Pro) | **Android 10 full unstripped** incl. `/apex/com.android.runtime`, `hwservicemanager`, `vndbinder`, HIDL HALs | budget fits Tier-2 Jetsam headroom |
| Any, manual `android-10` on 3 GB | allowed with clamped heaps (`-Xmx192m` SS) + aggressive `madvise` swap-eviction | user accepts OOM-kill risk; logged loudly |

## Heap & framebuffer tuning emitted per profile

| Profile | zygote/ART opts | system_server | fb resolution (iPad 9 2160×1620 native) |
|---|---|---|---|
| 7.1.2 @3 GB | `-Xms128m -Xmx256m` | `-Xms96m -Xmx384m` | 1620×1215 (0.75×; ~44 % framebuffer VRAM saved) |
| 7.1.2 @4 GB+ | `-Xms192m -Xmx320m` | `-Xms128m -Xmx448m` | native |
| 8.1 @3 GB | `-Xms160m -Xmx320m` | `-Xms112m -Xmx448m` | 1620×1215 |
| 10 (Tier 2) | `-Xms256m -Xmx512m` | `-Xms192m -Xmx640m` | native 2160×1620 |

## Memory-pressure ladder (module 7)

| iPadOS signal | Action |
|---|---|
| `DISPATCH_MEMORYPRESSURE_WARN` | `onTrimMemory(TRIM_MEMORY_RUNNING_LOW)` → app domains via binder snooper; `Runtime.gc()` + `VMRuntime.trimHeap()` per runtime; unpinned ashmem `MADV_FREE_REUSABLE`; cold `.oat` pages (>30 s) `MADV_DONTNEED` |
| `DISPATCH_MEMORYPRESSURE_CRITICAL` | `TRIM_MEMORY_RUNNING_CRITICAL`; full purge; all `.oat` cold pages evicted |
| watchdog: `os_proc_available_memory() < 220 MB` | emergency trim every 250 ms; ordered domain shutdown before iOS kills the process |
