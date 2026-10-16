#!/usr/bin/env bash
# Builds the .deb for this machine's architecture, on Debian or Ubuntu with
# a C/C++ compiler, libsdl2-dev and nfpm (https://nfpm.goreleaser.com).
#
#   packaging/build-deb.sh                 # version from git
#   VERSION=1.2.3 packaging/build-deb.sh
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
. packaging/version.sh

for tool in nfpm dpkg-shlibdeps; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "build-deb.sh: $tool not found; see packaging/README.md" >&2
    exit 1
  fi
done
ARCH="${ARCH:-$(dpkg --print-architecture)}"

PREFIX=/usr GXEMUL_VERSION="$VERSION" ./configure
make -j"$(nproc)"
rm -rf dist/stage
make install DESTDIR="$PWD/dist/stage"
gzip -9n dist/stage/usr/share/man/man1/gxemul.1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/debian"
printf 'Source: gxemul\n\nPackage: gxemul\nArchitecture: any\n' > "$work/debian/control"
root=$PWD
depends=$(cd "$work" && dpkg-shlibdeps -O -e "$root/dist/stage/usr/bin/gxemul" | sed -n 's/^shlibs:Depends=//p')
{
  cat packaging/nfpm.yaml
  echo "depends:"
  echo "$depends" | tr ',' '\n' | sed -e 's/^ *//' -e 's/^/  - "/' -e 's/$/"/'
} > "$work/nfpm.yaml"

VERSION="$VERSION" ARCH="$ARCH" nfpm pkg --config "$work/nfpm.yaml" --packager deb \
  --target "dist/gxemul-rp_${VERSION}_${ARCH}.deb"
ls -l "dist/gxemul-rp_${VERSION}_${ARCH}.deb"
