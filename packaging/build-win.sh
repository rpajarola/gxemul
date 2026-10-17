#!/usr/bin/env bash
# Builds a zip of gxemul for 64-bit Windows (10 and later), with SDL2.
#
# Natively, in an MSYS2 UCRT64 shell (pacman -S mingw-w64-ucrt-x86_64-gcc
# make zip), or cross-compiled with mingw-w64 elsewhere (brew install
# mingw-w64, or apt install gcc-mingw-w64-x86-64), where configure runs its
# test programs with $CROSS_RUN (default: wine).
#
#   packaging/build-win.sh                 # version from git
#   VERSION=1.2.3 packaging/build-win.sh
#   CROSS_RUN="wine --bottle gxemul" packaging/build-win.sh   # CrossOver
#
# SDL2 is the official mingw build, downloaded (or $SDL2_MINGW, an
# unpacked SDL2-devel-<version>-mingw directory).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
. packaging/version.sh

SDL2_VERSION=2.32.10
SDL2_SHA256=83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

case "$(uname -s)" in
  MINGW*|MSYS*)
    export CC="${CC:-gcc}" CC_FOR_BUILD="${CC_FOR_BUILD:-gcc}"
    run=()
    ;;
  *)
    export CC="${CC:-x86_64-w64-mingw32-gcc}" CC_FOR_BUILD="${CC_FOR_BUILD:-cc}"
    export CROSS_RUN="${CROSS_RUN:-wine}"
    if ! command -v "${CROSS_RUN%% *}" > /dev/null; then
      echo "Set CROSS_RUN to a command that runs Windows programs (e.g. wine)" >&2
      exit 1
    fi
    read -r -a run <<< "$CROSS_RUN"
    ;;
esac
OBJDUMP="${OBJDUMP:-${CC%gcc}objdump}"

if [ -z "${SDL2_MINGW:-}" ]; then
  tarball="SDL2-devel-$SDL2_VERSION-mingw.tar.gz"
  curl -fsSL -o "$work/$tarball" \
    "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/$tarball"
  echo "$SDL2_SHA256  $work/$tarball" | sha256sum -c - 2>/dev/null ||
    echo "$SDL2_SHA256  $work/$tarball" | shasum -a 256 -c -
  tar xzf "$work/$tarball" -C "$work"
  SDL2_MINGW="$work/SDL2-$SDL2_VERSION"
fi
sdl="$SDL2_MINGW/x86_64-w64-mingw32"

SDL2_CONFIG="$sdl/bin/sdl2-config" GXEMUL_VERSION="$VERSION" ./configure
grep -q '^#define WITH_SDL' config.h
make clean
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Only Windows' own DLLs and SDL2.dll may be needed:
"$OBJDUMP" -p gxemul.exe | sed -n 's/^.*DLL Name: //p' > "$work/dlls"
if grep -viE '^(kernel32|user32|ws2_32|iphlpapi|comdlg32|sdl2|api-ms-win-crt-[a-z0-9-]+)\.dll$' "$work/dlls"; then
  echo "gxemul.exe needs the DLLs above, which aren't in the zip" >&2
  exit 1
fi

name="gxemul-rp-$VERSION-windows-x86_64"
dir="$work/$name"
mkdir -p "$dir/doc"
cp gxemul.exe "$sdl/bin/SDL2.dll" "$dir/"
for f in README LICENSE HISTORY; do cp "$f" "$dir/$f.txt"; done
cp "$SDL2_MINGW/LICENSE.txt" "$dir/SDL2-LICENSE.txt"
cp -R doc/* "$dir/doc/"

# gxemul -V prints the version, then fails for want of a machine:
banner=$(cd "$dir" && ${run[@]+"${run[@]}"} ./gxemul.exe -V 2>&1 || true)
grep -q "GXemul $VERSION" <<< "$banner"

mkdir -p dist
out="$PWD/dist/$name.zip"
rm -f "$out"
(cd "$work" && zip -qr "$out" "$name")
ls -l "$out"
