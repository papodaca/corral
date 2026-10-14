#!/usr/bin/env bash
# Start the uninstalled binary from the source tree. The meson `run` target
# sets GSETTINGS_SCHEMA_DIR to the compiled schema in the build directory.
set -euo pipefail

exe=${1:?corral binary required}
shift

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "${MESON_SOURCE_ROOT:-${root}}"
exec "${exe}" "$@"
