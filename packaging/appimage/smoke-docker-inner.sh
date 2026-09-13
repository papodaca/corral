#!/usr/bin/env bash
# Runs inside ubuntu:26.04 with the repo bind-mounted.
set -euo pipefail

ROOT=${IT_DOCKER_ROOT:-/workspace}

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl \
  build-essential \
  pkg-config git \
  meson ninja-build valac \
  python3 xz-utils \
  libgtk-4-dev libadwaita-1-dev \
  libxml2-dev libpng-dev zlib1g-dev libbz2-dev \
  gobject-introspection \
  gir1.2-gtk-4.0 gir1.2-adw-1 \
  libglib2.0-bin librsvg2-bin librsvg2-common \
  desktop-file-utils \
  file patchelf \
  dpkg-dev findutils \
  sudo

apt-get install -y --no-install-recommends libgirepository-2.0-dev \
  || apt-get install -y --no-install-recommends libgirepository-1.0-dev \
  || true

apt-get install -y --no-install-recommends libgtk-4-bin || true
apt-get install -y --no-install-recommends libgdk-pixbuf-2.0-bin \
  || apt-get install -y --no-install-recommends libgdk-pixbuf2.0-bin \
  || true

useradd -m builder
echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
host_uid=$(stat -c %u "${ROOT}")
host_gid=$(stat -c %g "${ROOT}")
restore_ownership() {
  chown -R "${host_uid}:${host_gid}" "${ROOT}"
}
trap restore_ownership EXIT
chown -R builder:builder "${ROOT}"

cd "${ROOT}/packaging/appimage"
sudo -u builder env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  APPIMAGE_EXTRACT_AND_RUN=1 \
  HOME=/home/builder \
  IT_DOCKER_ROOT="${ROOT}" \
  GITHUB_REF="${GITHUB_REF:-}" \
  GITHUB_REF_NAME="${GITHUB_REF_NAME:-}" \
  GITHUB_REF_TYPE="${GITHUB_REF_TYPE:-}" \
  ./build.sh

shopt -s nullglob
images=("${ROOT}"/packaging/appimage/*.AppImage)
if [[ ${#images[@]} -eq 0 ]]; then
  echo "No AppImage produced" >&2
  ls -la "${ROOT}/packaging/appimage" >&2
  exit 1
fi
for img in "${images[@]}"; do
  echo "Built $(basename "${img}") ($(du -h "${img}" | awk '{print $1}'))"
done
