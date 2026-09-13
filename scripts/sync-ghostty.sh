#!/usr/bin/env bash
# Clone Ghostty v1.3.1 into third_party/ghostty and apply the GTK embed patch.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEST="${ROOT}/third_party/ghostty"
PATCH="${ROOT}/patches/ghostty-gtk-embed.patch"
TAG=v1.3.1

if [[ ! -f ${DEST}/include/ghostty.h ]]; then
  git clone --depth 1 --branch "${TAG}" https://github.com/ghostty-org/ghostty.git "${DEST}"
fi

if ! grep -q GHOSTTY_PLATFORM_GTK "${DEST}/include/ghostty.h"; then
  git -C "${DEST}" apply "${PATCH}"
fi

echo "Ghostty sources ready at ${DEST}"
