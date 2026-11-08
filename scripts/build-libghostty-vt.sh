#!/usr/bin/env bash
# Fetch and build libghostty-vt from its release tarballs.
# Usage:
#   build-libghostty-vt.sh fetch DEST
#   build-libghostty-vt.sh build DEST PREFIX STAMP_HEADER [A_COPY]
# DEST keeps the unpacked source and deps trees. A later configure or an
# offline build reuses them without a network fetch.
set -euo pipefail

VERSION=1.0.0
BASE_URL=https://release.files.ghostty.org/libghostty-vt/${VERSION}
SRC_SHA256=4a6ef758be721923e1969e7b81b443dcc92d8dd3fb5a39600c03a8817a753676
DEPS_SHA256=4d5ea5e448e4c7ea8fb55ccbaa8668f2a97d44f438cee831e2b7ecd162d85c29
MINISIGN_KEY=RWQlAjJC23149WL2sEpT/l0QKy7hMIFhYdQOFy0Z7z7PbneUgvlsnYcV

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

usage() {
  echo "usage: $0 fetch DEST | build DEST PREFIX STAMP_HEADER [A_COPY]" >&2
  exit 2
}

find_zig() {
  if [[ -n ${ZIG:-} && -x ${ZIG} ]]; then
    printf '%s' "${ZIG}"
    return
  fi
  local candidate ver=
  for candidate in \
    "${ROOT}/third_party/zig-linux/zig" \
    /tmp/corral-tools/zig-x86_64-linux-0.17.0/zig \
    "$(command -v zig || true)"; do
    if [[ -z ${candidate} || ! -x ${candidate} ]]; then
      continue
    fi
    ver=$("${candidate}" version 2>/dev/null || true)
    if [[ ${ver} == 0.17.* ]]; then
      printf '%s' "${candidate}"
      return
    fi
  done
  echo "Need Zig 0.17 on PATH or ZIG= (got ${ver:-none})" >&2
  echo "Run scripts/fetch-zig.sh first." >&2
  exit 1
}

src_dir() {
  printf '%s/libghostty-vt-%s' "$1" "${VERSION}"
}

deps_dir() {
  printf '%s/libghostty-vt-%s-deps' "$1" "${VERSION}"
}

unpacked() {
  local dest=$1
  [[ -f $(src_dir "${dest}")/LIB_VERSION ]] &&
    [[ $(<"$(src_dir "${dest}")/LIB_VERSION") == "${VERSION}" ]] &&
    [[ -d $(deps_dir "${dest}") ]]
}

fetch_one() {
  local dir=$1 name=$2 sha=$3
  local file="${dir}/${name}"
  if [[ ! -f ${file} ]] || ! echo "${sha}  ${file}" | sha256sum -c --quiet - 2>/dev/null; then
    echo "Downloading ${BASE_URL}/${name}" >&2
    curl -fL --retry 3 -o "${file}.part" "${BASE_URL}/${name}"
    mv -f "${file}.part" "${file}"
  fi
  echo "${sha}  ${file}" | sha256sum -c --quiet -
  if command -v minisign > /dev/null; then
    curl -fsL --retry 3 -o "${file}.minisig" "${BASE_URL}/${name}.minisig"
    minisign -Vq -P "${MINISIGN_KEY}" -m "${file}"
  fi
}

do_fetch() {
  local dest=$1
  if unpacked "${dest}"; then
    return
  fi
  mkdir -p "${dest}"
  fetch_one "${dest}" "libghostty-vt-${VERSION}.tar.gz" "${SRC_SHA256}"
  fetch_one "${dest}" "libghostty-vt-${VERSION}-deps.tar.gz" "${DEPS_SHA256}"
  rm -rf "$(src_dir "${dest}")" "$(deps_dir "${dest}")"
  tar -C "${dest}" -xzf "${dest}/libghostty-vt-${VERSION}.tar.gz"
  tar -C "${dest}" -xzf "${dest}/libghostty-vt-${VERSION}-deps.tar.gz"
  rm -f "${dest}"/libghostty-vt-"${VERSION}"*.tar.gz*
  unpacked "${dest}"
}

do_build() {
  local dest=$1 prefix=$2 stamp=$3 copy=${4:-}
  do_fetch "${dest}"
  local src deps zig_bin
  src=$(src_dir "${dest}")
  deps=$(deps_dir "${dest}")
  zig_bin=$(find_zig)

  # Zig 0.17 can abort with "reached unreachable code" when the global cache
  # already holds a package archive. Never reuse the caller's cache.
  local cache="${prefix}.zig-cache"
  rm -rf "${cache}" "${prefix}"
  mkdir -p "${cache}" "${prefix}"

  # zig build defaults to the host CPU, which SIGILLs on older machines.
  local zig_cpu
  case $(uname -m) in
    x86_64) zig_cpu=x86_64_v3 ;;
    aarch64 | arm64) zig_cpu=baseline ;;
    *)
      echo "No portable Zig CPU pin for $(uname -m)" >&2
      exit 1
      ;;
  esac

  (
    cd "${src}"
    # The unpacked tarball is not a git repo, so git walks up to Corral.
    # A Corral release tag then fails Ghostty's "must match build.zig" check.
    ZIG_GLOBAL_CACHE_DIR="${cache}" "${zig_bin}" build \
      --prefix "${prefix}" \
      --cache-dir "${cache}/local" \
      --system "${deps}" \
      -Dversion-string=1.4.1 \
      -Doptimize=ReleaseFast \
      -Dcpu="${zig_cpu}"
  )
  rm -rf "${cache}"

  if [[ ! -f ${prefix}/lib/libghostty-vt.a ]]; then
    echo "libghostty-vt.a missing after zig build" >&2
    ls -la "${prefix}/lib" >&2 || true
    exit 1
  fi

  cat > "${stamp}" << EOF
#pragma once
#define CORRAL_LIBGHOSTTY_VT_BUILT "${VERSION}"
EOF

  if [[ -n ${copy} ]]; then
    cp -f "${prefix}/lib/libghostty-vt.a" "${copy}"
  fi
}

case ${1:-} in
  fetch)
    [[ $# -eq 2 ]] || usage
    do_fetch "$2"
    printf '%s\n' "$(src_dir "$2")"
    ;;
  build)
    [[ $# -ge 4 && $# -le 5 ]] || usage
    shift
    do_build "$@"
    ;;
  *)
    usage
    ;;
esac
