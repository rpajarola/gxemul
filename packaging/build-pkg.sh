#!/usr/bin/env bash
# Builds a macOS installer package, on macOS with the Xcode command line
# tools and Homebrew's libx11 (brew install libx11 pkgconf). libX11 and the
# libraries it needs go into the package; the X server is XQuartz.
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
packaging/bundle-dylibs.sh "$root/usr/local/lib/gxemul" "$root/usr/local/bin/gxemul"
install -d "$root/usr/local/share/gxemul"
install -m 0755 packaging/macos/uninstall.sh "$root/usr/local/share/gxemul/uninstall.sh"
# gxemul -V prints the version, then fails for want of a machine:
banner=$("$root/usr/local/bin/gxemul" -V 2>&1 || true)
grep -q "GXemul $VERSION" <<< "$banner"

pkgbuild --root "$root" --identifier io.github.rpajarola.gxemul --version "$VERSION" \
  --install-location / --ownership recommended "$work/gxemul.pkg"
mkdir -p dist
arch=$(uname -m)
out="dist/gxemul-$VERSION-$arch.pkg"
sign=()
[ -n "${PKG_SIGN_IDENTITY:-}" ] && sign=(--sign "$PKG_SIGN_IDENTITY")
productbuild --package "$work/gxemul.pkg" ${sign[@]+"${sign[@]}"} "$out"
ls -l "$out"
