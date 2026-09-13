#!/usr/bin/env bash
# Build a Corral AppImage from a git checkout.
#
# Usage:
#   cd packaging/appimage
#   ./build.sh
#
# Produces: Corral-$VERSION-$ARCH.AppImage in this directory
# (ARCH is uname -m: x86_64 or aarch64).
# Requires Ubuntu 26.04-class deps: meson, ninja, valac, pkg-config,
# GTK4/libadwaita, libxml2, libpng, zlib, bzip2, librsvg2-bin, curl, file,
# patchelf, python3, xz-utils.
# Zig 0.15.2 is fetched by scripts/fetch-zig.sh.
#
# AppImages built on Ubuntu 26.04 target that glibc floor (GTK 4.22 / libadwaita 1.9).
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
HOST_ARCH=$(uname -m)

git config --global --add safe.directory "${REPO_ROOT}" 2>/dev/null || true
git -C "${REPO_ROOT}" config --global --add safe.directory '*' 2>/dev/null || true

case "${HOST_ARCH}" in
  x86_64|aarch64) ;;
  *)
    echo "AppImage packaging supports x86_64 and aarch64 (got: ${HOST_ARCH})" >&2
    exit 1
    ;;
esac

LINUXDEPLOY_VERSION=${LINUXDEPLOY_VERSION:-1-alpha-20251107-1}
LINUXDEPLOY_BIN="linuxdeploy-${HOST_ARCH}.AppImage"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/${LINUXDEPLOY_BIN}"
GTK_PLUGIN_URL="https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/7a3fbc31a9e5/linuxdeploy-plugin-gtk.sh"

CACHE_DIR=${LINUXDEPLOY_CACHE_DIR:-"${SCRIPT_DIR}/.linuxdeploy-cache"}
APPDIR="${SCRIPT_DIR}/AppDir"
BUILDDIR="${SCRIPT_DIR}/builddir"

appimage_version() {
  IT_DOCKER_ROOT="${REPO_ROOT}" "${REPO_ROOT}/packaging/version.sh"
}

patch_gtk_plugin() {
  # Ubuntu 26.04 / GTK 4.22+: no /usr/lib/.../gtk-4.0 modules tree, and
  # gdk-pixbuf no longer ships a 2.10.0/loaders dir (built-in loaders).
  # Upstream linuxdeploy-plugin-gtk still assumes both exist.
  local gtk=$1
  local tmp

  if ! grep -q 'SKIP_MISSING_GTK_MODULES' "${gtk}"; then
    tmp=$(mktemp)
    awk '
      BEGIN { patched = 0 }
      {
        if (!patched && $0 ~ /for elem in "\$\{src\[@\]\}"; do/) {
          print
          getline nextline
          if (nextline ~ /LD_GTK_LIBRARY_PATH/) {
            print "        # SKIP_MISSING_GTK_MODULES"
            print "        if [ ! -e \"$elem\" ]; then"
            print "            echo \"Skipping missing path: $elem\""
            print "            continue"
            print "        fi"
            print nextline
            patched = 1
            next
          }
          print nextline
          next
        }
        print
      }
      END {
        if (!patched) {
          print "patch_gtk_plugin: failed to locate copy_lib_tree loop" > "/dev/stderr"
          exit 1
        }
      }
    ' "${gtk}" >"${tmp}"
    mv "${tmp}" "${gtk}"
  fi

  if ! grep -q 'SKIP_MISSING_PIXBUF_LOADERS' "${gtk}"; then
    tmp=$(mktemp)
    python3 - "${gtk}" "${tmp}" <<'PY'
import sys
from pathlib import Path
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
text = src.read_text()
old = '''if [ -x "$gdk_pixbuf_query" ]; then
    echo "Updating pixbuf cache in $APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
    "$gdk_pixbuf_query" > "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
else
    echo "WARNING: gdk-pixbuf-query-loaders not found"
fi
if [ ! -f "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}" ]; then
    echo "WARNING: loaders.cache file is missing"
fi
sed -i "s|$gdk_pixbuf_moduledir/||g" "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"'''
new = '''# SKIP_MISSING_PIXBUF_LOADERS
if [ -d "$gdk_pixbuf_binarydir" ] && [ -x "$gdk_pixbuf_query" ]; then
    echo "Updating pixbuf cache in $APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
    mkdir -p "$(dirname "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}")"
    "$gdk_pixbuf_query" > "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
    sed -i "s|$gdk_pixbuf_moduledir/||g" "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"
elif [ ! -d "$gdk_pixbuf_binarydir" ]; then
    echo "WARNING: gdk-pixbuf loaders dir missing (built-in loaders); not setting GDK_PIXBUF_MODULE_FILE"
    sed -i "/GDK_PIXBUF_MODULE_FILE/d" "$HOOKFILE"
else
    echo "WARNING: gdk-pixbuf-query-loaders not found"
fi'''
if old not in text:
    raise SystemExit('patch_gtk_plugin: gdk-pixbuf cache block not found')
dst.write_text(text.replace(old, new, 1))
PY
    mv "${tmp}" "${gtk}"
  fi

  if ! grep -q 'SKIP_MISSING_RPATH_DIRS' "${gtk}"; then
    tmp=$(mktemp)
    python3 - "${gtk}" "${tmp}" <<'PY'
import sys
from pathlib import Path
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
text = src.read_text()
old = '''for directory in "${PATCH_ARRAY[@]}"; do
    while IFS= read -r -d '' file; do
        ln $verbose -sf "${file/$LD_GTK_LIBRARY_PATH\\//}" "$APPDIR/usr/lib"
    done < <(find "$directory" -name '*.so' -print0)
done'''
new = '''# SKIP_MISSING_RPATH_DIRS
for directory in "${PATCH_ARRAY[@]}"; do
    if [ -z "$directory" ] || [ ! -d "$directory" ]; then
        continue
    fi
    while IFS= read -r -d '' file; do
        ln $verbose -sf "${file/$LD_GTK_LIBRARY_PATH\\//}" "$APPDIR/usr/lib"
    done < <(find "$directory" -name '*.so' -print0)
done'''
if old not in text:
    raise SystemExit('patch_gtk_plugin: rpath loop not found')
dst.write_text(text.replace(old, new, 1))
PY
    mv "${tmp}" "${gtk}"
  fi

  chmod +x "${gtk}"
}

fetch_tooling() {
  mkdir -p "${CACHE_DIR}"
  local ld="${CACHE_DIR}/${LINUXDEPLOY_BIN}"
  local gtk="${CACHE_DIR}/linuxdeploy-plugin-gtk.sh"

  if [[ ! -f ${ld} ]]; then
    echo "Downloading linuxdeploy ${LINUXDEPLOY_VERSION} (${HOST_ARCH})…"
    curl -fL --retry 3 -o "${ld}.partial" "${LINUXDEPLOY_URL}"
    mv "${ld}.partial" "${ld}"
  fi
  if [[ ! -f ${gtk} ]]; then
    echo "Downloading linuxdeploy-plugin-gtk…"
    curl -fL --retry 3 -o "${gtk}.partial" "${GTK_PLUGIN_URL}"
    mv "${gtk}.partial" "${gtk}"
  fi
  patch_gtk_plugin "${gtk}"
  chmod +x "${ld}" "${gtk}"

  if [[ ! -x ${CACHE_DIR}/squashfs-root/plugins/linuxdeploy-plugin-appimage/usr/bin/appimagetool ]]; then
    echo "Extracting linuxdeploy (for appimagetool)…"
    rm -rf "${CACHE_DIR}/squashfs-root"
    (
      cd "${CACHE_DIR}"
      APPIMAGE_EXTRACT_AND_RUN=1 "./${LINUXDEPLOY_BIN}" --appimage-extract >/dev/null
    )
  fi
}

strip_graphics_driver_libs() {
  local patterns=(
    'libvulkan.so*'
    'libvulkan_*.so*'
    'libVkLayer*.so*'
    'libGLX_mesa.so*'
    'libEGL_mesa.so*'
    'libgallium*.so*'
    'libdrm_amdgpu.so*'
    'libdrm_radeon.so*'
    'libdrm_intel.so*'
    'libdrm_nouveau.so*'
    'libnvidia-*.so*'
    'libcuda.so*'
  )
  local pat
  for pat in "${patterns[@]}"; do
    find "${APPDIR}" -type f -name "${pat}" -delete 2>/dev/null || true
    find "${APPDIR}" -type l -name "${pat}" -delete 2>/dev/null || true
  done
  rm -rf \
    "${APPDIR}/usr/lib/dri" \
    "${APPDIR}/usr/share/vulkan" \
    "${APPDIR}/usr/lib/vulkan" \
    2>/dev/null || true
  find "${APPDIR}" -type d -name dri -exec rm -rf {} + 2>/dev/null || true
}

patch_apprun_hooks() {
  local hook="${APPDIR}/apprun-hooks/linuxdeploy-plugin-gtk.sh"
  if [[ ! -f ${hook} ]]; then
    return
  fi
  sed -i '/^export GDK_BACKEND=x11/d' "${hook}"
  sed -i '/^export GTK_THEME=/d' "${hook}"
}

# --- main -------------------------------------------------------------------

VERSION=$(appimage_version)
OUTPUT_NAME="Corral-${VERSION}-${HOST_ARCH}.AppImage"
OUTPUT_PATH="${SCRIPT_DIR}/${OUTPUT_NAME}"

echo "Building ${OUTPUT_NAME} from ${REPO_ROOT}"

rm -rf "${APPDIR}" "${BUILDDIR}"
rm -f "${SCRIPT_DIR}/Corral-"*-"${HOST_ARCH}.AppImage"
mkdir -p "${APPDIR}"

export ZIG
ZIG=$("${REPO_ROOT}/scripts/fetch-zig.sh")
"${REPO_ROOT}/scripts/sync-ghostty.sh"

fetch_tooling

meson setup "${BUILDDIR}" "${REPO_ROOT}" --prefix=/usr --buildtype=release \
  -Dpackage_version="${VERSION}"
meson compile -C "${BUILDDIR}"
meson install -C "${BUILDDIR}" --destdir "${APPDIR}"
glib-compile-schemas "${APPDIR}/usr/share/glib-2.0/schemas"

DESKTOP_FILE="${APPDIR}/usr/share/applications/dev.corral.Corral.desktop"
ICON_SVG="${REPO_ROOT}/data/icons/hicolor/scalable/apps/dev.corral.Corral.svg"
ICON_FILE="${APPDIR}/usr/share/icons/hicolor/512x512/apps/dev.corral.Corral.png"
if [[ ! -f ${DESKTOP_FILE} ]]; then
  echo "Missing desktop file after install: ${DESKTOP_FILE}" >&2
  exit 1
fi
if [[ ! -f ${ICON_SVG} ]]; then
  echo "Missing app icon: ${ICON_SVG}" >&2
  exit 1
fi
mkdir -p "$(dirname "${ICON_FILE}")"
rsvg-convert -w 512 -h 512 "${ICON_SVG}" -o "${ICON_FILE}"

export APPIMAGE_EXTRACT_AND_RUN=1
export DEPLOY_GTK_VERSION=4

LINUXDEPLOY="${CACHE_DIR}/squashfs-root/usr/bin/linuxdeploy"
if [[ ! -x ${LINUXDEPLOY} ]]; then
  LINUXDEPLOY="${CACHE_DIR}/${LINUXDEPLOY_BIN}"
fi
plugin_dest="$(dirname "${LINUXDEPLOY}")/linuxdeploy-plugin-gtk.sh"
if [[ ${plugin_dest} != "${CACHE_DIR}/linuxdeploy-plugin-gtk.sh" ]]; then
  cp -f "${CACHE_DIR}/linuxdeploy-plugin-gtk.sh" "${plugin_dest}"
fi
chmod +x "${plugin_dest}"

(
  cd "${SCRIPT_DIR}"
  env APPIMAGE_EXTRACT_AND_RUN=1 \
    DEPLOY_GTK_VERSION=4 \
    "${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --executable "${APPDIR}/usr/bin/corral" \
    --desktop-file "${DESKTOP_FILE}" \
    --icon-file "${ICON_FILE}" \
    --plugin gtk \
    --exclude-library='libvulkan.so*' \
    --exclude-library='libGLX_mesa.so*' \
    --exclude-library='libEGL_mesa.so*' \
    --exclude-library='libgallium*.so*' \
    --exclude-library='libnvidia-*.so*'
)

patch_apprun_hooks
strip_graphics_driver_libs

APPIMAGETOOL="${CACHE_DIR}/squashfs-root/plugins/linuxdeploy-plugin-appimage/usr/bin/appimagetool"
if [[ ! -x ${APPIMAGETOOL} ]]; then
  echo "appimagetool missing after linuxdeploy extract: ${APPIMAGETOOL}" >&2
  exit 1
fi
(
  cd "${SCRIPT_DIR}"
  env ARCH="${HOST_ARCH}" \
    VERSION="${VERSION}" \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    "${APPIMAGETOOL}" "${APPDIR}" "${OUTPUT_PATH}"
)

if [[ ! -f ${OUTPUT_PATH} ]]; then
  echo "AppImage not produced: expected ${OUTPUT_PATH}" >&2
  ls -la "${SCRIPT_DIR}" >&2 || true
  exit 1
fi
chmod +x "${OUTPUT_PATH}"

echo "AppImage written to ${OUTPUT_PATH}"
ls -lh "${OUTPUT_PATH}"
