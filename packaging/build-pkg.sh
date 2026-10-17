#!/usr/bin/env bash
# Builds a macOS installer package, on macOS with the Xcode command line
# tools and Homebrew's SDL2 (brew install sdl2 pkgconf). SDL2 and the
# libraries it needs go into the package.
#
#   packaging/build-pkg.sh                 # version from git
#   VERSION=1.2.3 packaging/build-pkg.sh
#
# The package is for the architecture of the Homebrew it was built with.
# Set PKG_SIGN_IDENTITY to a "Developer ID Installer" identity to sign it.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
. packaging/version.sh

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root="$work/root"

PREFIX=/usr/local GXEMUL_VERSION="$VERSION" ./configure
make clean
# Room in the load commands for the bundled libraries' paths:
make -j"$(sysctl -n hw.ncpu)" OTHERLIBS="-Wl,-headerpad_max_install_names"
make install DESTDIR="$root"
libdir="$root/usr/local/lib/gxemul"
packaging/bundle-dylibs.sh "$libdir" "$root/usr/local/bin/gxemul"
# Homebrew's SDL2 is sdl2-compat, which loads SDL3 when it starts, from
# its own directory (with dlopen, which bundle-dylibs.sh doesn't see):
if strings "$libdir"/libSDL2*.dylib | grep -q 'libSDL3\.dylib'; then
  cp "$(pkg-config --variable=libdir sdl3)/libSDL3.dylib" "$libdir/libSDL3.dylib"
  chmod u+w "$libdir/libSDL3.dylib"
  install_name_tool -id @loader_path/libSDL3.dylib "$libdir/libSDL3.dylib"
  packaging/bundle-dylibs.sh "$libdir" "$libdir/libSDL3.dylib"
fi
install -d "$root/usr/local/share/gxemul"
install -m 0755 packaging/macos/uninstall.sh "$root/usr/local/share/gxemul/uninstall.sh"
# gxemul -h prints the version (without a machine, gxemul would open
# the machines window):
banner=$("$root/usr/local/bin/gxemul" -h 2>&1 || true)
grep -q "GXemul $VERSION" <<< "$banner"

pkgbuild --root "$root" --identifier io.github.rpajarola.gxemul --version "$VERSION" \
  --install-location / --ownership recommended "$work/gxemul.pkg"
mkdir -p dist
arch=$(uname -m)
out="dist/gxemul-rp-$VERSION-$arch.pkg"
sign=()
[ -n "${PKG_SIGN_IDENTITY:-}" ] && sign=(--sign "$PKG_SIGN_IDENTITY")
productbuild --package "$work/gxemul.pkg" ${sign[@]+"${sign[@]}"} "$out"
ls -l "$out"
