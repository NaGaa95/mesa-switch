#!/bin/bash
set -e

IMAGE_NAME="devkitpro-mesa-rust"
CONTAINER_NAME="mesa-switch-build"

# Build the Docker image if needed.
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "=== Building Docker image ==="
    docker build -f Docker.rust -t "$IMAGE_NAME" .
fi

# Start the build container.
docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
docker run -d --name "$CONTAINER_NAME" \
    -v "$(pwd):/project" --workdir "/project" \
    "$IMAGE_NAME" sleep infinity

run() { docker exec "$CONTAINER_NAME" bash -c "$1"; }

# Install cross-build wrappers for bindgen and Rust compiler probes.
run '
mkdir -p /usr/local/libexec
cp /project/bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp /project/rustc-switch-wrapper.sh /usr/local/libexec/rustc
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
'

# Preserve the Meson path recorded in build.ninja for later regeneration.
run '
MESON_BIN="$(command -v meson || true)"
if [ -z "$MESON_BIN" ]; then
    echo "ERROR: meson not found in container PATH" >&2
    exit 1
fi
mkdir -p /usr/local/bin
if [ "$MESON_BIN" != "/usr/local/bin/meson" ]; then
    ln -sf "$MESON_BIN" /usr/local/bin/meson
fi
'

# Install this checkout's Nouveau headers for consumers and bindgen.
run '
cp /project/src/gallium/winsys/nouveau/drm/nouveau.h \
    /opt/devkitpro/portlibs/switch/include/
cp /project/src/nouveau/headers/nv_device_info.h \
    /opt/devkitpro/portlibs/switch/include/
'

# Alias Debian's full-version Clang headers to the major path mesa_clc uses.
run '
if [ ! -d /usr/lib/llvm-15/lib/clang/15/include ]; then
    ln -sf /usr/lib/llvm-15/lib/clang/15.0.6/include /usr/lib/llvm-15/lib/clang/15/include
fi
'

# Rust std requires -lrt -ldl -lutil. Supply placeholder archives;
# src/nouveau/vulkan/rust_switch_stubs.c provides compatibility symbols.
run '
if [ ! -f /opt/devkitpro/portlibs/switch/lib/libdl.a ]; then
    cd /tmp
    echo "void __mesa_switch_stub_lib(void) {}" > stub_posix.c
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -c stub_posix.c -o stub_posix.o
    for L in dl rt util; do
        /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs \
            /opt/devkitpro/portlibs/switch/lib/lib${L}.a stub_posix.o
    done
fi
'

# Build native host tools.
echo "=== Building native host tools (mesa_clc, vtn_bindgen2) ==="
run '
cd /project && meson setup builddir-native --wipe \
    -Dvulkan-drivers= \
    -Dgallium-drivers= \
    -Dshader-cache=true \
    -Dplatforms= \
    -Dglx=disabled \
    -Degl=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dtools=[] \
    -Dllvm=enabled \
    -Dmesa-clc=enabled \
    -Dprecomp-compiler=enabled \
    -Dinstall-mesa-clc=true
'
run 'ninja -C /project/builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2'

# Configure the cross build.
echo "=== Configuring cross build (Switch + nouveau + nouveau_vk) ==="
run '
export PATH="/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"
cd /project && meson setup builddir-switch --wipe \
    --cross-file switch_cross_file.txt \
    --buildtype=release \
    -Doptimization=1 \
    -Db_lto=false \
    -Db_ndebug=true \
    -Dvulkan-drivers=nouveau \
    -Dgallium-drivers=nouveau \
    -Dshader-cache=true \
    -Dgallium-rusticl=false \
    -Dplatforms=switch \
    -Dglx=disabled \
    -Degl=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dllvm=disabled \
    -Dshared-glapi=disabled \
    -Dshared-llvm=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dcpp_rtti=false
'

# Build Switch archives.
echo "=== Building Mesa for Switch ==="
run '
export PATH="/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"
ninja -C /project/builddir-switch \
    src/nouveau/vulkan/libnvk.a \
    src/nouveau/vulkan/libvulkan.a \
    src/vulkan/util/libvulkan_util.a \
    src/compiler/nir/libnir.a \
    src/compiler/libcompiler.a \
    src/compiler/spirv/libvtn.a \
    src/util/libxmlconfig.a \
    src/nouveau/compiler/libnak.a \
    src/nouveau/compiler/libnak_rs.a \
    src/nouveau/nil/libnil.a \
    src/nouveau/nil/liblibnil_format_table.a \
    src/nouveau/mme/libnouveau_mme.a \
    src/nouveau/winsys/libnouveau_ws.a \
    src/nouveau/headers/libnvidia_headers_c.a \
    src/compiler/rust/libcompiler_c_helpers.a \
    src/util/libmesa_util.a \
    src/util/libmesa_util_simd.a \
    src/util/blake3/libblake3.a \
    src/c11/impl/libmesa_util_c11.a
'

echo "=== Build complete ==="
docker stop "$CONTAINER_NAME"
