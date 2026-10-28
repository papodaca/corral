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

# Zig 0.17 takes the cache from ZIG_GLOBAL_CACHE_DIR. --global-cache-dir
# after `fetch` is rejected. Download with curl: zig's own HTTP client
# has failed CI with "unable to unpack tarball: ReadFailed".
while read -r url; do
  [[ -z ${url} ]] && continue
  base=$(basename "${url}")
  ok=0
  for attempt in 1 2 3; do
    echo "zig fetch ${url} (try ${attempt})"
    tmp=$(mktemp -d)
    if curl -fL --retry 3 --retry-delay 2 -o "${tmp}/${base}" "${url}" \
      && "${ZIG_BIN}" fetch "${tmp}/${base}"; then
      ok=1
      rm -rf "${tmp}"
      break
    fi
    rm -rf "${tmp}"
  done
  if [[ ${ok} -eq 0 ]]; then
    echo "zig fetch failed: ${url}" >&2
    exit 1
  fi
done < "${urls}"

echo "Zig package cache ready at ${CACHE}"
