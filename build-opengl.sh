#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/builddir-opengl}"
DESTDIR="${DESTDIR:-${ROOT_DIR}/mesa-install}"
MESON_BIN="${MESON:-meson}"
NINJA_BIN="${NINJA:-ninja}"
BUILD_TYPE="${MESA_BUILD_TYPE:-release}"
OPTIMIZATION="${MESA_OPTIMIZATION:-2}"
SHADER_CACHE="${MESA_SHADER_CACHE:-enabled}"
XMLCONFIG="${MESA_XMLCONFIG:-auto}"
EXPAT="${MESA_EXPAT:-auto}"
GLES1="${MESA_GLES1:-enabled}"
GLES2="${MESA_GLES2:-enabled}"

if [[ -n "${MSYSTEM:-}" ]]; then
    default_cross_file="${ROOT_DIR}/switch_cross_file_msys2.txt"
else
    default_cross_file="${ROOT_DIR}/switch_cross_file.txt"
fi
default_native_file=""

CROSS_FILE="${CROSS_FILE:-${default_cross_file}}"
NATIVE_FILE="${NATIVE_FILE:-${default_native_file}}"
cd "${ROOT_DIR}"

command -v "${MESON_BIN}" >/dev/null
command -v "${NINJA_BIN}" >/dev/null
command -v sed >/dev/null

export NINJA="${NINJA_BIN}"

setup_mode=()
if [[ -f "${BUILD_DIR}/meson-private/coredata.dat" ]]; then
    setup_mode+=(--wipe)
fi

native_args=()
if [[ -n "${NATIVE_FILE}" ]]; then
    native_args+=(--native-file "${NATIVE_FILE}")
fi

"${MESON_BIN}" setup "${BUILD_DIR}" "${setup_mode[@]}" \
    --cross-file "${CROSS_FILE}" \
    "${native_args[@]}" \
    --default-library=static \
    --prefix=/opt/devkitpro/portlibs/switch \
    --libdir=lib \
    --buildtype="${BUILD_TYPE}" \
    -Doptimization="${OPTIMIZATION}" \
    -Db_lto=false \
    -Db_ndebug=true \
    -Dvulkan-drivers= \
    -Dgallium-drivers=nouveau \
    -Dgallium-rusticl=false \
    -Dplatforms=switch \
    -Degl-native-platform=switch \
    -Dglx=disabled \
    -Degl=enabled \
    -Dopengl=true \
    -Dgles1="${GLES1}" \
    -Dgles2="${GLES2}" \
    -Dvideo-codecs= \
    -Dshader-cache="${SHADER_CACHE}" \
    -Dxmlconfig="${XMLCONFIG}" \
    -Dexpat="${EXPAT}" \
    -Dtools=[] \
    -Dllvm=disabled \
    -Dshared-llvm=disabled \
    -Dcpp_rtti=false \
    -Dbuild-tests=false

"${NINJA_BIN}" -C "${BUILD_DIR}"
"${MESON_BIN}" install -C "${BUILD_DIR}" --destdir "${DESTDIR}"

SDK_DIR="${DESTDIR}/opt/devkitpro/portlibs/switch"
for pc_file in "${SDK_DIR}"/lib/pkgconfig/*.pc; do
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "${pc_file}"
done

echo "Mesa Switch OpenGL SDK staged at:"
echo "  ${SDK_DIR}"
