#!/usr/bin/env bash
# Copy Ghostty's Zig package tarballs into ZIG_GLOBAL_CACHE_DIR.
# Nested bubblewrap (Flatpak) often cannot resolve DNS even with --share=network.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SRC="${ROOT}/third_party/ghostty"
CACHE=${ZIG_GLOBAL_CACHE_DIR:-${ROOT}/third_party/zig-cache}

if [[ -n ${ZIG:-} && -x ${ZIG} ]]; then
  ZIG_BIN=${ZIG}
else
  ZIG_BIN=$("${ROOT}/scripts/fetch-zig.sh")
fi

if [[ ! -f ${SRC}/build.zig.zon ]]; then
  echo "Ghostty sources missing at ${SRC}. Run scripts/sync-ghostty.sh" >&2
  exit 1
fi

mkdir -p "${CACHE}"
export ZIG_GLOBAL_CACHE_DIR="${CACHE}"

urls=$(mktemp)
trap 'rm -f "${urls}"' EXIT

find "${SRC}" -path '*/example/*' -prune -o -name '*.zig.zon' -print0 \
  | xargs -r -0 grep -hE '^[[:space:]]*\.url[[:space:]]*=' \
  | grep -oE 'https://[^"]+' \
  | grep -E '\.(tar\.(gz|xz|zst)|tgz)$' \
  | sort -u > "${urls}"

while read -r url; do
  [[ -z ${url} ]] && continue
  echo "zig fetch ${url}"
  "${ZIG_BIN}" fetch --global-cache-dir "${CACHE}" "${url}"
done < "${urls}"

echo "Zig package cache ready at ${CACHE}"
