#!/usr/bin/env bash
# Local smoke: build a .pkg.tar.zst inside archlinux:base-devel (mirrors release.yml).
# Usage: ./smoke-docker.sh
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)

docker run --rm \
  -e IT_DOCKER_ROOT=/workspace \
  -e GITHUB_REF \
  -e GITHUB_REF_NAME \
  -e GITHUB_REF_TYPE \
  -v "${REPO_ROOT}:/workspace" \
  -w /workspace \
  archlinux:base-devel \
  bash -euo pipefail /workspace/packaging/arch/smoke-docker-inner.sh
