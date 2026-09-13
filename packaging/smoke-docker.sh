#!/usr/bin/env bash
# Local Docker smoke for Arch, AppImage, and Flatpak (same images as release.yml).
#
# Usage:
#   ./smoke-docker.sh              # all, in order
#   ./smoke-docker.sh arch
#   ./smoke-docker.sh appimage
#   ./smoke-docker.sh flatpak
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
  cat >&2 <<'EOF'
usage: smoke-docker.sh [all|arch|appimage|flatpak]
EOF
  exit 1
}

TARGET=${1:-all}
case "${TARGET}" in
  all|arch|appimage|flatpak) ;;
  -h|--help) usage ;;
  *) usage ;;
esac

run_one() {
  local name=$1
  echo
  echo "################################################################"
  echo "# ${name}"
  echo "################################################################"
  "${SCRIPT_DIR}/${name}/smoke-docker.sh"
}

failures=0
if [[ ${TARGET} == all ]]; then
  for t in arch appimage flatpak; do
    if ! run_one "${t}"; then
      echo "FAIL: ${t}" >&2
      failures=$((failures + 1))
      break
    fi
    echo "PASS: ${t}"
  done
else
  if ! run_one "${TARGET}"; then
    echo "FAIL: ${TARGET}" >&2
    failures=1
  else
    echo "PASS: ${TARGET}"
  fi
fi

echo
echo "---- artifacts ----"
ls -lh "${SCRIPT_DIR}"/arch/corral-*.pkg.tar.zst 2>/dev/null || echo "arch: (none)"
ls -lh "${SCRIPT_DIR}"/appimage/*.AppImage 2>/dev/null || echo "appimage: (none)"
ls -lh "${SCRIPT_DIR}"/flatpak/*.flatpak 2>/dev/null || echo "flatpak: (none)"

if [[ ${failures} -ne 0 ]]; then
  exit 1
fi
echo "OVERALL: PASS"
