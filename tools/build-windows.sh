#!/usr/bin/env bash
# build-windows.sh — DEFINITIVE Windows cross-compile/package script for a recomp game.
#
# This is the Windows counterpart to tools/build-linux.sh (Linux AppImage). It mirrors
# the same prod-vs-debug discipline:
#
#   prod  (default) — strips ALL developer tooling: no TCP debug server, no
#                     observability rings, no oracle backend. The shipped build.
#   debug           — compiles the TCP debug server + rings back in.
#
# It uses the system-installed MinGW-w64 cross-compiler (x86_64-w64-mingw32-gcc)
# and fetches SDL3 for MinGW, configures + builds with cmake, then wraps the
# .exe + runtime DLLs into a self-contained Windows x64 ZIP. State policy
# (identical to the Linux AppImage, where everything lives next to the exe):
# config.ini, keybinds.ini, rom.cfg, saves/ and mods/ all live NEXT TO the .exe.
# The engine anchors there itself.
#
# After packaging, the ZIP contains the complete runtime ready to run on any
# Windows x64 machine (Win10+).
#
# Usage:
#   bash tools/build-windows.sh                     # prod ZIP (default)
#   bash tools/build-windows.sh --version 0.10.0    # stamp + name a release build
#   bash tools/build-windows.sh --config debug      # debug build (TCP server + rings)
#   bash tools/build-windows.sh --regen             # regen src/gen first (tools/regen.sh)
#   bash tools/build-windows.sh --auto-regen        # regen only if recomp config changed
#   bash tools/build-windows.sh --run               # not supported cross-platform (skip)
#   bash tools/build-windows.sh --no-package        # configure + build only, skip ZIP
#   bash tools/build-windows.sh --out DIR           # where to drop the .zip
#   bash tools/build-windows.sh --jobs N            # parallel build jobs (default: nproc)
#   bash tools/build-windows.sh --sdl3-url URL      # override SDL3 MinGW download URL
#
# NOTE on --jobs: the generated banks are multi-MB translation units compiled at
# -O3. On a memory-constrained host, too many concurrent jobs makes the compiler
# die with no diagnostic. Lower --jobs before suspecting the sources.
#
# Widescreen is a host custom renderer; generation never adds guest widening
# hooks. Windows and Linux consume the same generated guest source tree.
#
# Prereqs: cmake, ninja, curl, unzip (or python3), git, x86_64-w64-mingw32-gcc,
#          x86_64-w64-mingw32-g++, x86_64-w64-mingw32-windres.
# The SDL3 MinGW development package is fetched and SHA-verified for reproducible builds.

set -euo pipefail

# ============================ PER-GAME CONFIG ===============================
APP_NAME="SuperMetroid"
RELEASE_SLUG="SuperMetroidSNESRecomp"        # matches the Linux AppImage prefix
CMAKE_TARGET="SuperMetroidSNESRecomp"
ROM_EXTS="sfc smc"
EXTRA_ARGS=""
REGEN_CMD="bash tools/regen.sh"
PREBUILD_CMD=""
POSTBUILD_CMD=""
BOXART="recomp/launcher/boxart.tga"          # unused for Windows (kept for parity)
EXTRA_PAYLOAD=()                             # repo-relative files -> zip root/
# Widescreen and presentation FPS use the compiled-in Mods provider. They do
# not need external package manifests staged alongside the executable.
REQUIRED_MOD_MANIFESTS=()
PROD_CMAKE_FLAGS=( -DSNESRECOMP_ENABLE_TRACE=OFF )
DEBUG_CMAKE_FLAGS=( -DSNESRECOMP_ENABLE_TRACE=ON )
# ============================================================================

# SDL3 MinGW download (pinned to match framework's SNESRECOMP_SDL3_FETCH_VERSION)
SDL3_VERSION="3.4.10"
SDL3_MINGW_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-devel-${SDL3_VERSION}-mingw.tar.gz"
# SHA256 of SDL3-devel-3.4.10-mingw.tar.gz (NOT the source tarball)
SDL3_MINGW_SHA256="39dd2ac370bf33d6332a21ed768d8d49c37cc6f3211d788ead765102722639a8"

CONFIG="prod"
DO_REGEN=0
DO_RUN=0
DO_PACKAGE=1
VERSION=""
JOBS="$(nproc 2>/dev/null || echo 4)"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/release-windows"
SDL3_DL_DIR="$REPO/.cache/sdl3-mingw-dl"
SDL3_OUT_DIR="$REPO/.cache/sdl3-mingw"

DO_AUTO_REGEN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2;;
    --prod) CONFIG="prod"; shift;;
    --debug) CONFIG="debug"; shift;;
    --version) VERSION="$2"; shift 2;;
    --regen) DO_REGEN=1; shift;;
    --auto-regen) DO_AUTO_REGEN=1; shift;;
    --run) DO_RUN=1; shift;;
    --no-package) DO_PACKAGE=0; shift;;
    --out) OUT="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --sdl3-url) SDL3_MINGW_URL="$2"; shift 2;;
    -h|--help) sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
case "$CONFIG" in
  prod) FLAGS=( "${PROD_CMAKE_FLAGS[@]}" );;
  debug) FLAGS=( "${DEBUG_CMAKE_FLAGS[@]}" );;
  *) echo "--config must be prod or debug (got '$CONFIG')" >&2; exit 2;;
esac

# Default the version to the checked-out tag so a release build can't ship
# stamped "dev" by accident; explicit --version always wins.
if [ -z "$VERSION" ]; then
  VERSION="$(git -C "$REPO" describe --tags --exact-match 2>/dev/null | sed 's/^v//' || true)"
  [ -n "$VERSION" ] || VERSION="dev"
fi
FLAGS+=( -DSNESRECOMP_BUILD_VERSION="$VERSION" )

# SDL3 is the default; SNESRECOMP_SDL_BACKEND=SDL2 selects the compatibility
# package. We'll fetch SDL3 for MinGW.
SDL_BACKEND="${SNESRECOMP_SDL_BACKEND:-SDL3}"
FLAGS+=( -DSNESRECOMP_SDL_BACKEND="$SDL_BACKEND" )

# Static MinGW runtime by default (matches CI and setup-zip rebuild)
FLAGS+=( -DSNESRECOMP_STATIC_RUNTIME=ON )

# Single cleanup hook
WORK=""; RAN_PREBUILD=0
cleanup() {
  [ -n "$WORK" ] && rm -rf "$WORK"
  [ "$RAN_PREBUILD" = "1" ] && [ -n "$POSTBUILD_CMD" ] && { echo "[postbuild] $POSTBUILD_CMD"; eval "$POSTBUILD_CMD" || true; }
  return 0
}
trap cleanup EXIT

BUILD="$REPO/build-windows-$CONFIG"
echo "==================== $APP_NAME ($CONFIG, v$VERSION) ===================="
cd "$REPO"

[ -f "$REPO/snesrecomp/runner/runner.cmake" ] || {
  echo "ERROR: snesrecomp is not initialized; run 'git submodule update --init --recursive' first." >&2
  exit 1
}
[ -f "$REPO/recomp-ui/recomp_ui.cmake" ] || {
  echo "ERROR: recomp-ui is not initialized; run 'git submodule update --init --recursive' first." >&2
  exit 1
}

# Check for MinGW cross-compiler
for tool in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres; do
  command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: $tool not found; install mingw-w64" >&2; exit 1; }
done

# --- Smart regeneration check -----------------------------------------------
# If any recomp/*.cfg is newer than the oldest generated bank file, the
# generated code is stale. We can auto-regen (--auto-regen) or at least
# warn loudly so incremental builds don't silently use old code.
# Note: recomp/funcs.h is an OUTPUT of regen, not an input, so we don't
# check its timestamp (it updates every regen run even if banks are reused).
GEN_DIR="$REPO/src/gen"
CFG_DIR="$REPO/recomp"
NEEDS_REGEN=0
if [ -d "$GEN_DIR" ] && [ -d "$CFG_DIR" ]; then
  # Find oldest generated bank file (skip cache/manifest)
  OLDEST_GEN=$(find "$GEN_DIR" -maxdepth 1 -type f \( -name "bank*_v2.c" -o -name "dispatch_v2.c" \) -printf "%T@ %p\n" 2>/dev/null | sort -n | head -1 | cut -d' ' -f2-)
  if [ -n "$OLDEST_GEN" ] && [ -f "$OLDEST_GEN" ]; then
    # Check if any .cfg file is newer than that oldest gen file
    NEWER_CFG=$(find "$CFG_DIR" -maxdepth 1 -type f -name "*.cfg" -newer "$OLDEST_GEN" 2>/dev/null | head -1)
    if [ -n "$NEWER_CFG" ]; then
      NEEDS_REGEN=1
      echo "⚠️  Generated code appears stale: $NEWER_CFG is newer than $OLDEST_GEN"
    fi
  fi
fi

# Auto-regenerate if requested, or if --regen was passed, or if stale and --auto-regen
REGEN_RAN=0
if [ "$DO_REGEN" = "1" ] && [ -n "$REGEN_CMD" ]; then
  echo "[regen] $REGEN_CMD (explicit --regen)"
  eval "$REGEN_CMD"
  REGEN_RAN=1
elif [ "$DO_AUTO_REGEN" = "1" ] && [ "$NEEDS_REGEN" = "1" ] && [ -n "$REGEN_CMD" ]; then
  echo "[regen] $REGEN_CMD (auto-detected stale generated code)"
  eval "$REGEN_CMD"
  REGEN_RAN=1
elif [ "$NEEDS_REGEN" = "1" ]; then
  echo "ERROR: Generated code is out of date with recomp config." >&2
  echo "       Run with --regen to regenerate, or --auto-regen to do it automatically." >&2
  echo "       (This happens when you edit recomp/*.cfg files or need new HLE functions.)" >&2
  exit 1
fi

# If regeneration ran, touch the generated bank files so their timestamps
# reflect the regeneration time. This prevents false "stale" warnings on
# subsequent builds when v2_emit reuses existing output (no content change).
if [ "$REGEN_RAN" = "1" ]; then
  find "$GEN_DIR" -maxdepth 1 -type f \( -name "bank*_v2.c" -o -name "dispatch_v2.c" \) -exec touch {} + 2>/dev/null || true
fi

if [ -n "$PREBUILD_CMD" ]; then
  echo "[prebuild] $PREBUILD_CMD"
  RAN_PREBUILD=1
  eval "$PREBUILD_CMD"
fi

# ------------------------------------------------------------
# Fetch SDL3 for MinGW
# ------------------------------------------------------------
echo "[0/4] fetch SDL3 MinGW (${SDL3_VERSION})"
mkdir -p "$SDL3_DL_DIR" "$SDL3_OUT_DIR"

# Check if already fetched and verified
SDL3_ROOT="${SDL3_OUT_DIR}/SDL3-${SDL3_VERSION}/x86_64-w64-mingw32"
if [ -f "${SDL3_ROOT}/lib/cmake/SDL3/SDL3Config.cmake" ]; then
  echo "      SDL3 MinGW already present at ${SDL3_ROOT}"
else
  rm -rf "$SDL3_DL_DIR" "$SDL3_OUT_DIR"
  mkdir -p "$SDL3_DL_DIR" "$SDL3_OUT_DIR"

  echo "      fetching ${SDL3_MINGW_URL}"
  curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 \
    -o "${SDL3_DL_DIR}/SDL3-mingw.tar.gz" "${SDL3_MINGW_URL}"

  # Verify SHA256
  DL_SHA=$(sha256sum "${SDL3_DL_DIR}/SDL3-mingw.tar.gz" | awk '{print $1}')
  if [ "$DL_SHA" != "$SDL3_MINGW_SHA256" ]; then
    echo "ERROR: SDL3 MinGW tarball SHA256 mismatch" >&2
    echo "  expected: $SDL3_MINGW_SHA256" >&2
    echo "  got:      $DL_SHA" >&2
    exit 1
  fi

  echo "      unpacking SDL3 MinGW"
  tar -xzf "${SDL3_DL_DIR}/SDL3-mingw.tar.gz" -C "${SDL3_OUT_DIR}"
  rm -f "${SDL3_DL_DIR}/SDL3-mingw.tar.gz"
  SDL3_ROOT="${SDL3_OUT_DIR}/SDL3-${SDL3_VERSION}/x86_64-w64-mingw32"
  echo "      SDL3 MinGW ready at ${SDL3_ROOT}"
fi

# ------------------------------------------------------------
# Configure + Build using MinGW cross-compiler
# ------------------------------------------------------------
echo "[1/4] configure ($CONFIG: ${FLAGS[*]})"

# Create a toolchain file for CMake (persistent, not temp, so CMake can re-read it)
TOOLCHAIN_FILE="$BUILD/mingw-toolchain.cmake"
mkdir -p "$BUILD"
cat > "$TOOLCHAIN_FILE" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH ${SDL3_ROOT} /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

cmake -S "$REPO" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${SDL3_ROOT}" \
  "${FLAGS[@]}"

echo "[2/4] build ($CMAKE_TARGET, -j$JOBS)"
cmake --build "$BUILD" --target "$CMAKE_TARGET" -j"$JOBS"

# Locate the produced .exe
BIN=""
while IFS= read -r f; do
  if [ "$(basename "$f")" = "$CMAKE_TARGET.exe" ]; then
    # Verify it's a PE executable (use file if available, otherwise check magic bytes)
    if command -v file >/dev/null 2>&1; then
      file -b "$f" 2>/dev/null | grep -q "PE.*executable" && { BIN="$f"; break; }
    else
      # Fallback: check for PE magic bytes (MZ)
      head -c 2 "$f" 2>/dev/null | grep -q $'MZ' && { BIN="$f"; break; }
    fi
  fi
done < <(find "$BUILD" -maxdepth 3 -type f -name "$CMAKE_TARGET.exe")
[ -n "$BIN" ] || { echo "ERROR: no PE executable named '$CMAKE_TARGET.exe' under $BUILD" >&2; exit 1; }
echo "      EXE: $BIN ($(du -h "$BIN" | cut -f1))"

if [ "$DO_PACKAGE" = "0" ]; then echo "      (--no-package) done."; exit 0; fi

# ------------------------------------------------------------
# Package runtime ZIP
# ------------------------------------------------------------
echo "[3/4] package Windows ZIP"
mkdir -p "$OUT"
EXE="$(basename "$BIN")"
BUILD_DIR="$(dirname "$BIN")"

STAGE_DIR="$(mktemp -d)"
WORK="$STAGE_DIR"
trap 'rm -rf "$STAGE_DIR" "$TOOLCHAIN_FILE"' EXIT

echo "      staging exe -> $STAGE_DIR/$EXE"
cp "$BIN" "$STAGE_DIR/$EXE"

# Copy config.ini (seeded by CMake from tracked config.ini)
[ -f "$BUILD_DIR/config.ini" ] && cp "$BUILD_DIR/config.ini" "$STAGE_DIR/config.ini"

# Copy launcher assets/ (staged by CMake POST_BUILD)
if [ -d "$BUILD_DIR/assets" ]; then
  echo "      staging launcher assets/ -> $STAGE_DIR/assets"
  cp -r "$BUILD_DIR/assets" "$STAGE_DIR/assets"
fi

# Copy mods/ catalog if present (built-in mods)
if [ -d "$BUILD_DIR/mods" ]; then
  echo "      staging mod catalog -> $STAGE_DIR/mods"
  cp -r "$BUILD_DIR/mods" "$STAGE_DIR/mods"
fi

# Extra read-only payload
for rel in "${EXTRA_PAYLOAD[@]}"; do
  [ -f "$REPO/$rel" ] || { echo "ERROR: payload missing: $REPO/$rel" >&2; exit 1; }
  echo "      staging payload -> $STAGE_DIR/$(basename "$rel")"
  cp "$REPO/$rel" "$STAGE_DIR/$(basename "$rel")"
done

# Bundle runtime DLLs from the SDL3 MinGW package
# The exe is linked static (-static) so it only needs SDL3.dll if dynamically linked
# but we'll copy it anyway for safety, plus any other MinGW DLLs it might need.
if [ -f "${SDL3_ROOT}/bin/SDL3.dll" ]; then
  echo "      staging runtime DLL: SDL3.dll"
  cp "${SDL3_ROOT}/bin/SDL3.dll" "$STAGE_DIR/SDL3.dll"
fi

# Create the ZIP
ZIP_NAME="${RELEASE_SLUG}-${VERSION}-windows-x64.zip"
ZIP_PATH="$OUT/$ZIP_NAME"
rm -f "$ZIP_PATH"

echo "      creating $ZIP_PATH"
cd "$STAGE_DIR"
if command -v zip >/dev/null 2>&1; then
  zip -r -9 "$ZIP_PATH" . >/dev/null
elif command -v python3 >/dev/null 2>&1; then
  python3 -c '
import zipfile, sys, os
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
  for root, dirs, files in os.walk("."):
    for f in files:
      fp = os.path.join(root, f)
      arc = os.path.relpath(fp, ".")
      z.write(fp, arc)
' "$ZIP_PATH"
else
  echo "ERROR: neither zip nor python3 available for packaging" >&2
  exit 1
fi
cd "$REPO"

echo "      BUILT: $ZIP_PATH ($(du -h "$ZIP_PATH" | cut -f1))"

# Verify ZIP contents
echo "[4/4] verify ZIP contents"
unzip -l "$ZIP_PATH"

# SHA256 for release tracking
sha256sum "$ZIP_PATH"

if [ "$DO_RUN" = "1" ]; then
  echo "WARNING: --run not supported for cross-compiled Windows builds on Linux"
  echo "         Copy $ZIP_PATH to a Windows machine, extract, and run $EXE"
fi