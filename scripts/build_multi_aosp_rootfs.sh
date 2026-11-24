#!/usr/bin/env bash
# ============================================================================
#  AOTDroid-Adaptive-iOS  |  scripts/build_multi_aosp_rootfs.sh   (MODULE 6)
#
#  Builds compact, unstripped, AOT-precompiled rootfs packs for
#  Android 7.1.2 (API 25), Android 8.1.0 (API 27) and Android 10.0.0
#  (API 29) ARM64, using each release's OWN dex2oat binary:
#
#    7.1.2  : OAT 088, BOOTCLASSPATH jars under /system/framework
#    8.1.0  : OAT 131, same layout (Treble split optional at runtime)
#    10.0.0 : OAT 170, ART in /apex/com.android.runtime (javalib + dex2oat)
#
#  For every profile:
#    1. unpack system.img / super.img (debugfs -- no root, no loop mounts)
#    2. run the matching arm64 dex2oat across BOOTCLASSPATH and
#       SYSTEMSERVERCLASSPATH under qemu-aarch64-static on an x86_64 host
#       (or natively on arm64 hosts)
#    3. llvm-strip --strip-debug on every .so/.oat (DWARF out, symtab kept:
#       "unstripped" for backtraces, compact for the container)
#    4. freeze the static TLS layout for the whole link set (TSV)
#    5. run aotdroid-rewrite over every .so / .oat / executable:
#       16 KiB re-layout + svc/TPIDR veneers + Mach-O conversion
#    6. emit rootfs-android-<ver>.aotpack (read-only, 16K-aligned blobs)
#       + manifest.json with sha256s
#
#  Requirements (checked below): bash 4+, e2fsprogs (debugfs), qemu-user,
#  llvm-strip (llvm-14+), python3, and the aotdroid-rewrite host tool.
#
#  Inputs (any of):
#    SYSTEM_IMAGE_71 / SYSTEM_IMAGE_81 / SYSTEM_IMAGE_10   env vars or
#    --image <ver>=<path>   a raw or sparse system.img / super.img
#    (sparse images are converted with simg2img when available)
#
#  Everything lands in rootfs-out/ unless --out is given.
# ============================================================================
set -euo pipefail
shopt -s nullglob

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$REPO_ROOT/rootfs-out"
NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# ---------------------------------------------------------------------------
log()  { printf '\033[1;36m[aotdroid-rootfs]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[aotdroid-rootfs FATAL]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: build_multi_aosp_rootfs.sh [--profile 7.1|8.1|10|all] [--out DIR]
                                  [--image VER=PATH] [--jobs N]

  --profile   which AOSP release to build (default: all)
  --image     system.img / super.img for a version (repeatable);
              can also come from $SYSTEM_IMAGE_71 / _81 / _10
  --out       output directory (default: <repo>/rootfs-out)
  --jobs      parallel dex2oat jobs (default: nproc)

Canonical sources:
  8.1 / 10 GSIs : https://source.android.com/setup/build/gsi (ci.android.com
                  builds; set --image 8.1=<aosp_arm64-ab-img.zip contents>)
  7.1.2         : no GSI exists (pre-Treble). Build from source:
                    repo init -u https://android.googlesource.com/platform/manifest \
                              -b android-7.1.2_r39
                    repo sync -c -j8
                    lunch aosp_arm64-userdebug && m
                  then: --image 7.1=$OUT/target/product/aosp_arm64/system.img
EOF
}

# ---------------------------------------------------------------------------
PROFILE_SET=()
declare -A IMAGE_PATHS
while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) PROFILE_SET+=("$2"); shift 2 ;;
    --image)   [[ "$2" == *=* ]] || die "--image VER=PATH"
               IFS='=' read -r v p <<< "$2"; IMAGE_PATHS["$v"]="$p"; shift 2 ;;
    --out)     OUT="$2"; shift 2 ;;
    --jobs)    NPROC="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *)         usage; die "unknown option: $1" ;;
  esac
done
[[ ${#PROFILE_SET[@]} -eq 0 ]] && PROFILE_SET=(all)
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# dependency check
need() { command -v "$1" >/dev/null 2>&1 || MISSING+=("$1"); }
MISSING=()
need debugfs; need python3; need llvm-strip; need sha256sum
if [[ $(uname -m) == x86_64 ]]; then need qemu-aarch64-static; fi
[[ ${#MISSING[@]} -gt 0 ]] && die "missing tools: ${MISSING[*]}
  Debian/Ubuntu: apt-get install e2fsprogs qemu-user llvm clang python3"

[[ -x "$REPO_ROOT/build-host/aotdroid-rewrite" ]] || {
  log "building aotdroid-rewrite host tool first..."
  (cd "$REPO_ROOT" && ./scripts/host_build.sh >/dev/null)
}
REWRITE="$REPO_ROOT/build-host/aotdroid-rewrite"

# ---------------------------------------------------------------------------
# Per-profile table: profile -> (api, oat_version, boot_classpath_prefix,
#                                dex2oat_path, image_file_var)
profile_api()   { case "$1" in 7.1) echo 25;; 8.1) echo 27;; 10) echo 29;; esac; }
profile_oat()   { case "$1" in 7.1) echo 088;; 8.1) echo 131;; 10) echo 170;; esac; }
profile_bcp_root() {   # where BOOTCLASSPATH jars live inside the image
  case "$1" in
    7.1|8.1) echo "/system/framework" ;;
    10)      echo "/apex/com.android.runtime/javalib" ;;
  esac
}
profile_dex2oat() {
  case "$1" in
    7.1|8.1) echo "/system/bin/dex2oat" ;;
    10)      echo "/apex/com.android.runtime/bin/dex2oat" ;;
  esac
}
profile_image_var() { case "$1" in 7.1) echo SYSTEM_IMAGE_71;; 8.1) echo SYSTEM_IMAGE_81;; 10) echo SYSTEM_IMAGE_10;; esac; }

# BOOTCLASSPATH for each release (framework jars in dependency order)
profile_bootclasspath() {
  case "$1" in
    7.1) echo "/system/framework/core-libart.jar:/system/framework/conscrypt.jar:/system/framework/okhttp.jar:/system/framework/core-junit.jar:/system/framework/bouncycastle.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/system/framework/apache-xml.jar:/system/framework/org.apache.http.legacy.boot.jar" ;;
    8.1) echo "/system/framework/core-oj.jar:/system/framework/core-libart.jar:/system/framework/conscrypt.jar:/system/framework/okhttp.jar:/system/framework/bouncycastle.jar:/system/framework/apache-xml.jar:/system/framework/legacy-test.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/system/framework/android.test.base.jar" ;;
    10)  echo "/apex/com.android.runtime/javalib/core-oj.jar:/apex/com.android.runtime/javalib/core-libart.jar:/apex/com.android.runtime/javalib/conscrypt.jar:/apex/com.android.runtime/javalib/okhttp.jar:/apex/com.android.runtime/javalib/bouncycastle.jar:/apex/com.android.runtime/javalib/apache-xml.jar:/apex/com.android.runtime/javalib/legacy-test.jar:/apex/com.android.runtime/javalib/core-icu4j.jar:/system/framework/framework.jar:/system/framework/ext.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar" ;;
  esac
}
profile_sscp() {  # SYSTEMSERVERCLASSPATH
  case "$1" in
    7.1) echo "/system/framework/services.jar:/system/framework/ethernet-service.jar:/system/framework/wifi-service.jar" ;;
    8.1) echo "/system/framework/services.jar:/system/framework/ethernet-service.jar:/system/framework/wifi-service.jar:/system/framework/com.android.location.provider.jar" ;;
    10)  echo "/system/framework/services.jar:/system/framework/ethernet-service.jar:/system/framework/wifi-service.jar:/system/framework/com.android.location.provider.jar" ;;
  esac
}

# ---------------------------------------------------------------------------
# unpack an ext4 system image into a directory with debugfs (no root needed)
unpack_image() {   # $1 image path  $2 dest dir
  local img="$1" dest="$2"
  mkdir -p "$dest"
  # sparse -> raw
  if command -v simg2img >/dev/null 2>&1; then
    if head -c 4 "$img" | od -An -tx1 | tr -d ' \n' | grep -q '^3aff26ed'; then
      log "converting sparse image to raw: $img"
      simg2img "$img" "$img.raw" && img="$img.raw"
    fi
  fi
  # super.img? extract the system partition image
  if debugfs -R "stat /" "$img" >/dev/null 2>&1; then
    :
  else
    log "$img is not a plain ext4; trying as super.img (lpunpack)"
    if command -v lpunpack >/dev/null 2>&1; then
      local tmpd; tmpd="$(mktemp -d)"
      lpunpack "$img" "$tmpd"
      img="$(ls "$tmpd"/system.img 2>/dev/null || true)"
      [[ -n "$img" ]] || die "could not find system.img inside $img"
    else
      die "$img is neither ext4 nor unpackable super.img (install lpunpack)"
    fi
  fi
  log "extracting $img -> $dest (debugfs)"
  debugfs -R "rdump / $dest" "$img" >/dev/null 2>&1
  # debugfs rdump writes into dest/<original root>; normalize
  if [[ -d "$dest/system" && ! -d "$dest/framework" ]]; then
    mv "$dest/system/"* "$dest/" 2>/dev/null || true
    rmdir "$dest/system" 2>/dev/null || true
  fi
}

# ---------------------------------------------------------------------------
# run a command from the image's own user space under qemu (or natively)
run_in_image() {   # $1 root dir, rest = argv (paths INSIDE the image)
  local root="$1"; shift
  if [[ $(uname -m) == x86_64 ]]; then
    qemu-aarch64-static -L "$root" "$root/$1" "${@:2}"
  else
    (cd "$root" && "$@")
  fi
}

# ---------------------------------------------------------------------------
# dex2oat a jar set into a boot image (boot.art / boot.oat)
build_boot_image() {   # $1 profile, $2 root, $3 out dir (inside root), $4 = extra jars
  local prof="$1" root="$2" bcp sscp jars
  bcp="$(profile_bootclasspath "$prof")"
  sscp="$(profile_sscp "$prof")"
  local outdir="$root/$(dirname "$(profile_bcp_root "$prof")/arm64")"
  mkdir -p "$outdir"
  log "[$prof] dex2oat boot image -> ${outdir#$root}"

  # Build --dex-file list from the BCP (only jars that exist in the image)
  local dex_args=()
  IFS=':' read -ra jars <<< "$bcp"
  for j in "${jars[@]}"; do
    [[ -f "$root$j" ]] && dex_args+=(--dex-file="$root$j")
  done
  [[ ${#dex_args[@]} -gt 0 ]] || die "[$prof] no BOOTCLASSPATH jars found under $root"

  # The 64-bit boot image base for arm64 (art defaults; matches AOSP)
  local base=0x7000000000

  run_in_image "$root" \
    "$(profile_dex2oat "$prof")" \
    --runtime-arg -Xms256m --runtime-arg -Xmx512m \
    --image="$outdir/boot.art" \
    --offset-of-image=0 \
    --base=$base \
    --instruction-set=arm64 \
    --instruction-set-variant=generic \
    --instruction-set-features=default \
    --android-root="$root/system" \
    --compile-pic \
    --compiler-filter=speed \
    --runtime-arg -classpath:"$bcp" \
    "${dex_args[@]}"

  # services.jar -> services.odex (SYSTEMSERVERCLASSPATH)
  log "[$prof] dex2oat system_server (services.odex)"
  local sjar="$root/system/framework/services.jar"
  if [[ -f "$sjar" ]]; then
    run_in_image "$root" \
      "$(profile_dex2oat "$prof")" \
      --runtime-arg -Xms256m --runtime-arg -Xmx512m \
      --dex-file="$sjar" \
      --oat-file="$root/system/framework/oat/arm64/services.odex" \
      --instruction-set=arm64 \
      --instruction-set-variant=generic \
      --instruction-set-features=default \
      --android-root="$root/system" \
      --compile-pic \
      --compiler-filter=speed \
      --runtime-arg -classpath:"$sscp" \
      --boot-image="$outdir/boot.art"
  fi
}

# ---------------------------------------------------------------------------
# strip DWARF, keep symbol tables ("unstripped" services, compact footprint)
strip_debug() {   # $1 root
  local n=0
  while IFS= read -r -d '' f; do
    llvm-strip --strip-debug "$f" && n=$((n+1))
  done < <(find "$1" -type f \( -name '*.so' -o -name '*.oat' \
            -o -name '*.odex' -o -name '*.art' \) -print0)
  log "stripped DWARF from $n binaries (symtabs kept)"
}

# ---------------------------------------------------------------------------
# freeze the static TLS layout for the link set (soname -> byte offset)
# Model: each DSO's PT_TLS block gets a slot in the emulated per-thread TP
# area, 16-byte aligned, in deterministic load order.
write_tls_layout() {   # $1 root, $2 out.tsv
  python3 - "$1" "$2" <<'PYEOF'
import os, struct, sys
root, out = sys.argv[1], sys.argv[2]
TLS = 7
entries = []
sofiles = []
for dirp, _, files in os.walk(root):
    for f in files:
        if f.endswith('.so'):
            sofiles.append(os.path.join(dirp, f))
sofiles.sort()
off = 0
rows = []
for so in sofiles:
    with open(so, 'rb') as fh:
        data = fh.read()
    if len(data) < 64 or data[:4] != b'\x7fELF':
        continue
    e_phoff = struct.unpack_from('<Q', data, 0x20)[0]
    e_phentsize = struct.unpack_from('<H', data, 0x36)[0]
    e_phnum = struct.unpack_from('<H', data, 0x38)[0]
    for i in range(e_phnum):
        ph = data[e_phoff + i*e_phentsize : e_phoff + (i+1)*e_phentsize]
        if struct.unpack_from('<I', ph, 0)[0] == TLS:
            memsz = struct.unpack_from('<Q', ph, 40)[0]
            if memsz:
                rows.append((os.path.basename(so), off))
                off += (memsz + 15) & ~15
            break
with open(out, 'w') as fh:
    for name, o in rows:
        fh.write(f"{name} {o}\n")
print(f"tls layout: {len(rows)} DSOs, {off} bytes", file=sys.stderr)
PYEOF
}

# ---------------------------------------------------------------------------
# run aotdroid-rewrite across every code-bearing file; collect converted
# Mach-O objects into converted/<soname>.o plus an ld64 link manifest
rewrite_all() {   # $1 root, $2 profile, $3 out dir
  local root="$1" prof="$2" out="$3" api oatver
  api="$(profile_api "$prof")"; oatver="$(profile_oat "$prof")"
  mkdir -p "$out"
  local n=0
  while IFS= read -r -d '' f; do
    local rel="${f#"$root"}" obj
    obj="$out/$(echo "${rel#/}" | tr '/' '_').o"
    if "$REWRITE" --in "$f" --out-macho "$obj" \
        --soname "$(basename "$f")" \
        --page-size 16384 \
        --tls-layout "$OUT/tls-$prof.tsv" >/dev/null 2>"$OUT/rewrite.err"; then
      n=$((n+1))
    else
      log "WARN: rewrite failed for $rel: $(cat "$OUT/rewrite.err")"
    fi
  done < <(find "$root" -type f \( -name '*.so' -o -name '*.oat' -o -name '*.odex' \
            -o -path '*/bin/*' -o -path '*/etc/hw/*' \) -print0)
  log "[$prof] converted $n images to Mach-O"
}

# ---------------------------------------------------------------------------
# .aotpack writer: header + string table + entries + 16K-aligned blobs
pack_rootfs() {   # $1 root, $2 out.pack
  python3 - "$1" "$2" <<'PYEOF'
import os, struct, sys, hashlib
root, out = sys.argv[1], sys.argv[2]
PAGE = 16384
files = []
for dirp, _, names in os.walk(root):
    for f in names:
        full = os.path.join(dirp, f)
        rel = os.path.relpath(full, root)
        if rel.startswith('oat/') or '/oat/' in rel or rel.endswith('.art'):
            kind = 'code'
        elif rel.endswith(('.so', '.oat', '.odex')):
            kind = 'code'
        else:
            kind = 'data'
        files.append((full, rel, kind))
files.sort(key=lambda x: x[1])
strtab = bytearray(b'\x00')
entries = []
for full, rel, kind in files:
    if os.path.islink(full) or not os.path.isfile(full):
        continue
    entries.append((full, rel, kind, len(strtab)))
    strtab += rel.encode() + b'\x00'
# layout: header(32) | strtab | entries(40 each) | blobs
hdr_size = 40
entry_off = (hdr_size + len(strtab) + 7) & ~7
blob_off = (entry_off + 40*len(entries) + PAGE - 1) & ~(PAGE - 1)
blobs = bytearray()
metas = []
sha = hashlib.sha256()
for full, rel, kind, nameoff in entries:
    data = open(full, 'rb').read()
    sha.update(data)
    if kind == 'code':
        blob_off_cur = (blob_off + len(blobs) + PAGE - 1) & ~(PAGE - 1)
        blobs += b'\x00' * (blob_off_cur - blob_off - len(blobs))
    else:
        blob_off_cur = blob_off + len(blobs)
    blobs += data
    if len(data) & 7:
        blobs += b'\x00' * (8 - (len(data) & 7))
    metas.append((nameoff, blob_off_cur, len(data), kind))
with open(out, 'wb') as fh:
    fh.write(b'AOTPACK1')
    fh.write(struct.pack('<IIQQQ', 1, len(metas), hdr_size, entry_off, blob_off))
    fh.write(strtab)
    fh.write(b'\x00' * ((hdr_size + len(strtab) + 7 & ~7) - hdr_size - len(strtab)))
    fh.seek(entry_off)
    for nameoff, boff, size, kind in metas:
        mode = 0o755 if kind == 'code' else 0o644
        flags = 1 if kind == 'code' else 0
        fh.write(struct.pack('<IIIIQQII', nameoff, mode, 0, 0, boff, size, flags, 0))
    fh.seek(blob_off)
    fh.write(blobs)
print(f"pack: {len(metas)} entries, {len(blobs)} blob bytes, sha256={sha.hexdigest()[:16]}...",
      file=sys.stderr)
with open(out + '.sha256', 'w') as fh:
    fh.write(sha.hexdigest() + '\n')
PYEOF
}

# ---------------------------------------------------------------------------
build_profile() {
  local prof="$1"
  local api oatver var img root
  api="$(profile_api "$prof")"
  oatver="$(profile_oat "$prof")"
  var="$(profile_image_var "$prof")"
  img="${IMAGE_PATHS[$prof]:-${!var:-}}"
  [[ -n "$img" && -f "$img" ]] || { log "SKIP $prof: no image (set --image $prof=PATH or \$$var)"; return 0; }

  log "======== building Android $prof (API $api, OAT $oatver) ========"
  root="$OUT/root-$prof"
  rm -rf "$root"; mkdir -p "$root"
  unpack_image "$img" "$root"
  [[ -d "$root/system" || -d "$root/framework" ]] || die "unpack produced no /system"

  # 1. AOT compile with the release's own dex2oat
  build_boot_image "$prof" "$root"

  # 2. strip DWARF, keep symtabs (unstripped-but-compact services)
  strip_debug "$root"

  # 3. freeze TLS layout for the entire link set
  write_tls_layout "$root" "$OUT/tls-$prof.tsv"

  # 4. rewrite every code image (veneers + 16K + Mach-O)
  rewrite_all "$root" "$prof" "$OUT/converted-$prof"

  # 5. pack
  pack_rootfs "$root" "$OUT/rootfs-android-$prof.aotpack"
  log "[$prof] done: $OUT/rootfs-android-$prof.aotpack"
}

# ---------------------------------------------------------------------------
declare -A MANIFEST
for prof in "${PROFILE_SET[@]}"; do
  if [[ "$prof" == all ]]; then
    for p in 7.1 8.1 10; do build_profile "$p"; done
  else
    build_profile "$prof"
  fi
done

# final manifest
python3 - "$OUT" <<'PYEOF'
import json, os, sys, hashlib
out = sys.argv[1]
m = {"generator": "build_multi_aosp_rootfs.sh", "version": 1, "profiles": {}}
for tag, api, oat in (("android-7.1", 25, "088"), ("android-8.1", 27, "131"),
                      ("android-10", 29, "170")):
    pack = os.path.join(out, f"rootfs-{tag}.aotpack")
    if os.path.exists(pack):
        m["profiles"][tag] = {
            "api": api, "oat_version": oat, "pack": os.path.basename(pack),
            "pack_bytes": os.path.getsize(pack),
            "sha256": hashlib.sha256(open(pack, 'rb').read()).hexdigest(),
        }
json.dump(m, open(os.path.join(out, "manifest.json"), 'w'), indent=2)
print(json.dumps(m, indent=2))
PYEOF
log "ALL DONE -> $OUT"
