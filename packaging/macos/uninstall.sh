#!/bin/sh
# Removes gxemul installed from the .pkg:
#   sudo /usr/local/share/gxemul/uninstall.sh
set -e
if [ "$(id -u)" != 0 ]; then
  echo "run with sudo" >&2
  exit 1
fi
rm -f /usr/local/bin/gxemul /usr/local/share/man/man1/gxemul.1
rm -rf /usr/local/lib/gxemul /usr/local/share/doc/gxemul /usr/local/share/gxemul
pkgutil --forget io.github.rpajarola.gxemul >/dev/null 2>&1 || true
# (with the ROM images of gxemul-rp-roms, in /usr/local/share/gxemul/roms)
pkgutil --forget io.github.rpajarola.gxemul.roms >/dev/null 2>&1 || true
echo "gxemul removed"
