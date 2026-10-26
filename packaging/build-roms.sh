#!/usr/bin/env bash
# Builds gxemul-rp-roms, a package of ROM images for gxemul, from a
# directory of them. The images are not in this repository: they are the
# copyrighted firmware of the machines and cards, so package only images
# you may redistribute.
#
#   packaging/build-roms.sh deb DIR     # .deb (nfpm): /usr/share/gxemul/roms
#   packaging/build-roms.sh pkg DIR     # macOS .pkg: /usr/local/share/gxemul/roms
#
# Everything in DIR is installed as it is, so it can also hold MAME ROM
# sets (ds5k133.zip, or a ds5k133 directory). gxemul looks for ROM images
# given by name in those directories after the user's own ROM folders.
# The version is VERSION, or the date (YYYY.MM.DD).
set -euo pipefail

if [ $# -ne 2 ] || [ ! -d "$2" ] || { [ "$1" != deb ] && [ "$1" != pkg ]; }; then
  echo "usage: $0 deb|pkg DIR" >&2
  exit 1
fi
kind=$1
src=$(cd "$2" && pwd)
cd "$(dirname "${BASH_SOURCE[0]}")/.."
VERSION="${VERSION:-$(date +%Y.%m.%d)}"

if [ -z "$(find "$src" -type f ! -name '.*' | head -1)" ]; then
  echo "build-roms.sh: no files in $src" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p dist

if [ "$kind" = deb ]; then
  if ! command -v nfpm >/dev/null 2>&1; then
    echo "build-roms.sh: nfpm not found; see packaging/README.md" >&2
    exit 1
  fi
  cat > "$work/nfpm.yaml" <<YAML
name: gxemul-rp-roms
arch: all
platform: linux
version: "${VERSION}"
section: otherosfs
priority: optional
maintainer: "Rico Pajarola <rp@servium.ch>"
homepage: "https://github.com/rpajarola/gxemul"
recommends:
  - gxemul-rp
description: |
  ROM images for gxemul
  System and option ROM images, which gxemul finds by name in
  /usr/share/gxemul/roms, e.g. for running a DECstation's own firmware.
contents:
  - src: "${src}"
    dst: /usr/share/gxemul/roms
    type: tree
YAML
  out="dist/gxemul-rp-roms_${VERSION}_all.deb"
  nfpm pkg --config "$work/nfpm.yaml" --packager deb --target "$out"
else
  install -d "$work/root/usr/local/share/gxemul"
  cp -R "$src" "$work/root/usr/local/share/gxemul/roms"
  find "$work/root" -name '.*' -type f -delete
  chmod -R u=rwX,go=rX "$work/root"
  pkgbuild --root "$work/root" --identifier io.github.rpajarola.gxemul.roms \
    --version "$VERSION" --install-location / --ownership recommended \
    "$work/roms.pkg"
  out="dist/gxemul-rp-roms-$VERSION.pkg"
  sign=()
  [ -n "${PKG_SIGN_IDENTITY:-}" ] && sign=(--sign "$PKG_SIGN_IDENTITY")
  productbuild --package "$work/roms.pkg" ${sign[@]+"${sign[@]}"} "$out"
fi
ls -l "$out"
