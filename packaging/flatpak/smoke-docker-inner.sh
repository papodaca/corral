#!/usr/bin/env bash
# Runs inside ubuntu:latest with the repo bind-mounted.
set -euo pipefail

ROOT=${IT_DOCKER_ROOT:-/workspace}

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl gnupg xz-utils \
  flatpak flatpak-builder ostree elfutils \
  appstream git sudo

git config --global --add safe.directory "${ROOT}" 2>/dev/null || true

run_appstream_compose() {
  local files_root=$1
  GLYCIN_DISABLE_SANDBOX=1 appstreamcli compose \
    --prefix=/ \
    --origin=dev.corral.Corral \
    --result-root="${files_root}" \
    --data-dir="${files_root}/share/app-info/xmls" \
    --icons-dir="${files_root}/share/app-info/icons/flatpak" \
    --print-report=full \
    --components=dev.corral.Corral,dev.corral.Corral.desktop \
    "${files_root}"
}

compose_probe=$(mktemp -d)
bash "${ROOT}/packaging/install-data.sh" / "${compose_probe}/files"
run_appstream_compose "${compose_probe}/files"
rm -rf "${compose_probe}"

flatpak_version() {
  "${ROOT}/packaging/version.sh"
}

useradd -m builder
echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
host_uid=$(stat -c %u "${ROOT}")
host_gid=$(stat -c %g "${ROOT}")
restore_ownership() {
  chown -R "${host_uid}:${host_gid}" "${ROOT}"
}
trap restore_ownership EXIT
chown -R builder:builder "${ROOT}"

VERSION=$(flatpak_version)
export ROOT VERSION
sudo -u builder env HOME=/home/builder ROOT="${ROOT}" VERSION="${VERSION}" \
  bash -euo pipefail <<'EOF'
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install -y --user flathub \
  org.gnome.Platform//50 \
  org.gnome.Sdk//50
cd "${ROOT}/packaging/flatpak"
mkdir -p repo
if [[ ! -f repo/config ]]; then
  ostree init --repo=repo --mode=archive
fi
ostree --repo=repo config set core.min-free-space-percent 0
ostree --repo=repo config set core.min-free-space-size 1MB
flatpak-builder --user --force-clean --disable-rofiles-fuse --repo=repo build-dir \
  dev.corral.Corral.json
EOF

files_root="${ROOT}/packaging/flatpak/build-dir/files"
run_appstream_compose "${files_root}"
chown -R builder:builder "${files_root}/share/app-info"

sudo -u builder env HOME=/home/builder ROOT="${ROOT}" VERSION="${VERSION}" \
  bash -euo pipefail <<'EOF'
cd "${ROOT}/packaging/flatpak"
ostree --repo=repo config set core.min-free-space-percent 0
ostree --repo=repo config set core.min-free-space-size 1MB
flatpak build-export repo build-dir
flatpak build-bundle repo \
  "dev.corral.Corral-${VERSION}.flatpak" \
  dev.corral.Corral
EOF

shopt -s nullglob
bundles=("${ROOT}"/packaging/flatpak/*.flatpak)
if [[ ${#bundles[@]} -eq 0 ]]; then
  echo "No Flatpak bundle produced" >&2
  ls -la "${ROOT}/packaging/flatpak" >&2
  exit 1
fi
for b in "${bundles[@]}"; do
  echo "Built $(basename "${b}") ($(du -h "${b}" | awk '{print $1}'))"
done
