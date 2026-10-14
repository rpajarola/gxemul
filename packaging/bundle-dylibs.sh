#!/usr/bin/env bash
# Copies the libraries that programs need from outside macOS (Homebrew's,
# say) into a directory next to them, and points the programs and the
# copies at each other, so that an installer package works without those
# libraries installed. Everything changed is signed again (ad hoc), as
# arm64 requires.
#
#   packaging/bundle-dylibs.sh <libdir> <program>...
#
# libdir is where the copies go, e.g. root/usr/local/lib/gxemul for
# programs in root/usr/local/bin.
set -euo pipefail

libdir=$1
shift
mkdir -p "$libdir"
libdir=$(cd "$libdir" && pwd)

# deps prints the libraries a Mach-O file loads, one per line.
deps() {
  otool -L "$1" | tail -n +2 | awk '{print $1}'
}

# rpaths prints the LC_RPATH entries of a Mach-O file.
rpaths() {
  otool -l "$1" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'
}

# system reports whether a library comes with macOS.
system() {
  case "$1" in
    /usr/lib/* | /System/*) return 0 ;;
  esac
  return 1
}

# resolve prints the real path of library $1 loaded by $2, whose original
# location (for @loader_path) is $3.
resolve() {
  local lib=$1 file=$2 origin=$3 dir rp cand
  dir=$(dirname "$origin")
  case "$lib" in
    @loader_path/* | @executable_path/*)
      cand="$dir/${lib#*/}"
      ;;
    @rpath/*)
      cand=
      while read -r rp; do
        rp=${rp/@loader_path/$dir}
        rp=${rp/@executable_path/$dir}
        if [ -e "$rp/${lib#@rpath/}" ]; then
          cand="$rp/${lib#@rpath/}"
          break
        fi
      done < <(rpaths "$file")
      if [ -z "$cand" ]; then
        echo "bundle-dylibs.sh: cannot find $lib for $origin" >&2
        exit 1
      fi
      ;;
    *)
      cand=$lib
      ;;
  esac
  python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$cand"
}

# quiet runs a command without the warning that a signature becomes
# invalid; everything is signed again at the end.
quiet() {
  "$@" 2> >(grep -v "will invalidate the code signature" >&2)
}

# Work through the programs and every library they pull in. origin[]
# remembers where each copy came from, to resolve its @loader_path.
queue=()
origins=()
for p in "$@"; do
  queue+=("$p")
  origins+=("$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$p")")
done
done_list=" "
i=0
while [ $i -lt ${#queue[@]} ]; do
  file=${queue[$i]}
  origin=${origins[$i]}
  i=$((i + 1))
  chmod u+w "$file"
  case "$file" in
    "$libdir"/*) ref_dir=$libdir ;;
    *) ref_dir=$(cd "$(dirname "$file")" && pwd) ;;
  esac
  rel=$(python3 -c 'import os, sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$libdir" "$ref_dir")
  while read -r lib; do
    system "$lib" && continue
    real=$(resolve "$lib" "$file" "$origin")
    name=$(basename "$real")
    copy="$libdir/$name"
    if [ "${done_list#* $name }" = "$done_list" ]; then
      done_list="$done_list$name "
      cp "$real" "$copy"
      chmod u+w "$copy"
      quiet install_name_tool -id "@loader_path/$name" "$copy"
      queue+=("$copy")
      origins+=("$real")
    fi
    quiet install_name_tool -change "$lib" "@loader_path/$rel/$name" "$file"
  done < <(deps "$file" | grep -v "^$(otool -D "$file" | tail -n +2)\$" || true)
done

for f in "${queue[@]}"; do
  codesign --force --sign - "$f" 2>/dev/null
done
echo "bundle-dylibs.sh: $((${#queue[@]} - $#)) libraries in $libdir"
