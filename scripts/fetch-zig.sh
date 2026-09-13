#!/usr/bin/env bash
# Download Zig 0.15.2 into third_party/zig-linux if needed.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEST="${ROOT}/third_party/zig-linux"
VERSION=0.15.2
ARCH=$(uname -m)
case "${ARCH}" in
  x86_64|aarch64) ;;
  *)
    echo "Unsupported arch for bundled Zig: ${ARCH}" >&2
    exit 1
    ;;
esac

if [[ -x ${DEST}/zig ]]; then
  ver=$("${DEST}/zig" version || true)
  if [[ ${ver} == "${VERSION}" ]]; then
    printf '%s\n' "${DEST}/zig"
    exit 0
  fi
fi

tarball="zig-${ARCH}-linux-${VERSION}.tar.xz"
url="https://ziglang.org/download/${VERSION}/${tarball}"
tmp=$(mktemp -d)
trap 'rm -rf "${tmp}"' EXIT
echo "Downloading ${url}" >&2
curl -fL --retry 3 -o "${tmp}/${tarball}" "${url}"
mkdir -p "${DEST}"
tar -C "${tmp}" -xf "${tmp}/${tarball}"
rm -rf "${DEST}"
mv "${tmp}/zig-${ARCH}-linux-${VERSION}" "${DEST}"
printf '%s\n' "${DEST}/zig"
