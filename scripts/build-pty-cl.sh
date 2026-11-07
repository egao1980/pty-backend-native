#!/usr/bin/env bash
# Build libpty-cl into lib/<os>-<arch>/ with zig cc.
#   scripts/build-pty-cl.sh [os-arch ...]   default: the host
#   os-arch ∈ linux-amd64 linux-arm64 darwin-arm64 windows-amd64
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GLIBC="2.31"
die() { echo "build-pty-cl.sh: $*" >&2; exit 1; }
command -v zig >/dev/null 2>&1 || die "zig not found"

host_pair() {
  local os arch
  case "$(uname -s)" in Linux) os=linux ;; Darwin) os=darwin ;;
    MINGW*|MSYS*|CYGWIN*) os=windows ;; *) die "unknown host OS" ;; esac
  case "$(uname -m)" in x86_64|amd64) arch=amd64 ;; arm64|aarch64) arch=arm64 ;;
    *) die "unknown host arch" ;; esac
  echo "${os}-${arch}"
}

build() {
  local pair=$1 target out flags=(-O2 -shared -fvisibility=hidden -Wall -Wextra -Werror)
  out="${ROOT}/lib/${pair}"
  mkdir -p "$out"
  case "$pair" in
    linux-amd64) target="x86_64-linux-gnu.${GLIBC}" ;;
    linux-arm64) target="aarch64-linux-gnu.${GLIBC}" ;;
    darwin-arm64) target="aarch64-macos" ;;
    windows-amd64) target="x86_64-windows-gnu" ;;
    *) die "unsupported os-arch: $pair" ;;
  esac
  echo "==> ${pair} (${target})"
  case "$pair" in
    linux-*) zig cc -target "$target" "${flags[@]}" -Wl,-soname,libpty-cl.so \
               -o "$out/libpty-cl.so" "${ROOT}/native/pty-cl.c" ;;
    darwin-*) zig cc -target "$target" "${flags[@]}" -Wl,-install_name,@rpath/libpty-cl.dylib \
               -o "$out/libpty-cl.dylib" "${ROOT}/native/pty-cl.c" ;;
    windows-*) zig cc -target "$target" "${flags[@]}" -o "$out/pty-cl.dll" "${ROOT}/native/pty-cl.c"
               rm -f "$out"/*.lib "$out"/*.pdb ;;
  esac
  ls -la "$out"
}

pairs=("$@")
[[ ${#pairs[@]} -gt 0 ]] || pairs=("$(host_pair)")
for pair in "${pairs[@]}"; do build "$pair"; done
