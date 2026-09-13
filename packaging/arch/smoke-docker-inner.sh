#!/usr/bin/env bash
# Runs inside archlinux:base-devel with the repo bind-mounted.
set -euo pipefail

ROOT=${IT_DOCKER_ROOT:-/workspace}

pacman -Syu --noconfirm --needed \
  base-devel git sudo

git config --global --add safe.directory "${ROOT}" 2>/dev/null || true

useradd -m builder
echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
host_uid=$(stat -c %u "${ROOT}")
host_gid=$(stat -c %g "${ROOT}")
restore_ownership() {
  chown -R "${host_uid}:${host_gid}" "${ROOT}"
}
trap restore_ownership EXIT
chown -R builder:builder "${ROOT}"

cd "${ROOT}/packaging/arch"
sudo -u builder env \
  HOME=/home/builder \
  IT_DOCKER_ROOT="${ROOT}" \
  GITHUB_REF="${GITHUB_REF:-}" \
  GITHUB_REF_NAME="${GITHUB_REF_NAME:-}" \
  GITHUB_REF_TYPE="${GITHUB_REF_TYPE:-}" \
  makepkg -s --noconfirm

shopt -s nullglob
pkgs=("${ROOT}"/packaging/arch/corral-*.pkg.tar.zst)
copied=0
for pkg in "${pkgs[@]}"; do
  base=$(basename "${pkg}")
  case "${base}" in
    *-debug-*) continue ;;
  esac
  copied=$((copied + 1))
  echo "Built ${base} ($(du -h "${pkg}" | awk '{print $1}'))"
done
if [[ ${copied} -eq 0 ]]; then
  echo "No package produced" >&2
  ls -la "${ROOT}/packaging/arch" >&2
  exit 1
fi
