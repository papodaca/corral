#!/usr/bin/env bash
# Build libghostty (embedded runtime) as a static archive and copy artifacts.
# Usage: build-libghostty.sh GHOSTTY_SRC PREFIX STAMP_HEADER [A_COPY]
set -euo pipefail

SRC=${1:?ghostty source dir}
PREFIX=${2:?install prefix}
STAMP=${3:?stamp header}

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PATCH="${ROOT}/patches/ghostty-gtk-embed.patch"

find_zig() {
  if [[ -n ${ZIG:-} && -x ${ZIG} ]]; then
    printf '%s' "${ZIG}"
    return
  fi
  local candidate
  for candidate in \
    "${ROOT}/third_party/zig-linux/zig" \
    /tmp/corral-tools/zig-x86_64-linux-0.15.2/zig \
    "$(command -v zig || true)"; do
    if [[ -z ${candidate} || ! -x ${candidate} ]]; then
      continue
    fi
    ver=$("${candidate}" version 2>/dev/null || true)
    if [[ ${ver} == 0.15.2 ]]; then
      printf '%s' "${candidate}"
      return
    fi
  done
  echo "Need Zig 0.15.2 on PATH or ZIG= (got ${ver:-none})" >&2
  echo "Run scripts/fetch-zig.sh first." >&2
  exit 1
}

if [[ ! -f ${SRC}/include/ghostty.h ]]; then
  echo "Ghostty sources missing at ${SRC}. Run scripts/sync-ghostty.sh" >&2
  exit 1
fi

if [[ -f ${PATCH} ]] && ! grep -q GHOSTTY_PLATFORM_GTK "${SRC}/include/ghostty.h"; then
  git -C "${SRC}" apply "${PATCH}"
fi

ZIG_BIN=$(find_zig)
mkdir -p "${PREFIX}"
(
  cd "${SRC}"
  "${ZIG_BIN}" build \
    -Dapp-runtime=none \
    -Doptimize=ReleaseFast \
    -Demit-exe=false \
    -Demit-docs=false \
    -Demit-helpgen=false \
    -p "${PREFIX}"
)

if [[ ! -f ${PREFIX}/lib/libghostty.a ]]; then
  echo "libghostty.a missing after zig build" >&2
  ls -la "${PREFIX}/lib" >&2 || true
  exit 1
fi

# glslang/spirv-cross were built with Zig's libc++. Fold that runtime
# into the archive so gcc can link Corral without a system libc++.so.
fold_cxx_runtime() {
  local fat=$1
  local cache=${ZIG_GLOBAL_CACHE_DIR:-${HOME}/.cache/zig}
  local newest
  newest() {
    find "${cache}/o" -name "$1" -printf '%T@ %p\n' 2>/dev/null | sort -n | tail -1 | cut -d' ' -f2-
  }
  local libcxx libcxxabi libunwind
  libcxx=$(newest 'libc++.a')
  libcxxabi=$(newest 'libc++abi.a')
  libunwind=$(newest 'libunwind.a')
  if [[ -z ${libcxx} || -z ${libcxxabi} ]]; then
    echo "Zig libc++.a not in ${cache}/o; gcc link will need system libc++" >&2
    return 0
  fi
  local out="${fat}.with-cxx.a"
  rm -f "${out}"
  {
    printf 'CREATE %s\n' "${out}"
    printf 'ADDLIB %s\n' "${fat}"
    printf 'ADDLIB %s\n' "${libcxx}"
    printf 'ADDLIB %s\n' "${libcxxabi}"
    if [[ -n ${libunwind} ]]; then
      printf 'ADDLIB %s\n' "${libunwind}"
    fi
    printf 'SAVE\nEND\n'
  } | ar -M
  mv -f "${out}" "${fat}"
}

fold_cxx_runtime "${PREFIX}/lib/libghostty.a"

mkdir -p "${PREFIX}/share/ghostty"
if [[ -d ${SRC}/src/shell-integration ]]; then
  rm -rf "${PREFIX}/share/ghostty/shell-integration"
  cp -a "${SRC}/src/shell-integration" "${PREFIX}/share/ghostty/shell-integration"
fi

cat > "${STAMP}" <<'EOF'
#pragma once
#define CORRAL_LIBGHOSTTY_BUILT 1
EOF

if [[ $# -ge 4 ]]; then
  cp -f "${PREFIX}/lib/libghostty.a" "${4}"
fi
