#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <meson-build-directory> <output.AppImage>" >&2
  exit 2
fi

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(cd "$1" && pwd)"
OUTPUT_PATH="$2"
if [[ "$OUTPUT_PATH" != /* ]]; then
  OUTPUT_PATH="${SOURCE_DIR}/${OUTPUT_PATH}"
fi

if [[ ! -f "${BUILD_DIR}/meson-private/coredata.dat" ]]; then
  echo "Not a Meson build directory: ${BUILD_DIR}" >&2
  exit 1
fi

APPDIR="${BUILD_DIR}/AppDir"
TOOLS_DIR="${BUILD_DIR}/appimage-tools"
WORK_DIR="${BUILD_DIR}/appimage-output"
LINUXDEPLOY="${TOOLS_DIR}/linuxdeploy-x86_64.AppImage"

rm -rf "${APPDIR}" "${WORK_DIR}"
mkdir -p "${TOOLS_DIR}" "${WORK_DIR}" "$(dirname "${OUTPUT_PATH}")"

meson install -C "${BUILD_DIR}" --destdir "${APPDIR}" --skip-subprojects "*"

if [[ ! -x "${APPDIR}/usr/bin/aegisub" ]]; then
  echo "The installed Aegisub executable is missing from the AppDir." >&2
  exit 1
fi

if [[ ! -x "${LINUXDEPLOY}" ]]; then
  curl --fail --location --retry 3 \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
    --output "${LINUXDEPLOY}"
  chmod +x "${LINUXDEPLOY}"
fi

(
  cd "${WORK_DIR}"
  APPIMAGE_EXTRACT_AND_RUN=1 "${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --executable "${APPDIR}/usr/bin/aegisub" \
    --desktop-file "${APPDIR}/usr/share/applications/org.aegisub.Aegisub.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/scalable/apps/org.aegisub.Aegisub.svg" \
    --output appimage
)

mapfile -t images < <(find "${WORK_DIR}" -maxdepth 1 -type f -name '*.AppImage')
if [[ ${#images[@]} -ne 1 ]]; then
  echo "Expected one generated AppImage, found ${#images[@]}." >&2
  exit 1
fi

mv -f "${images[0]}" "${OUTPUT_PATH}"
chmod +x "${OUTPUT_PATH}"

rm -rf "${WORK_DIR}/squashfs-root"
(
  cd "${WORK_DIR}"
  "${OUTPUT_PATH}" --appimage-extract >/dev/null
  test -x squashfs-root/AppRun
  test -x squashfs-root/usr/bin/aegisub
)

echo "Created ${OUTPUT_PATH}"
