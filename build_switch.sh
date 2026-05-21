#!/bin/bash
set -e

if ! command -v meson &> /dev/null; then
  echo "Updating apt and installing dependencies..."
  apt-get update
  apt-get install -y python3-pip python3-setuptools python3-wheel ninja-build pkg-config flex bison curl libclang-dev clang
  pip3 install --break-system-packages meson mako
fi

echo "Fixing devkitPro pkg-config..."
sed -i 's/$(DEVKITPRO)/\/opt\/devkitpro/g' /opt/devkitpro/portlibs/switch/lib/pkgconfig/libdrm_nouveau.pc || true

echo "Configuring Mesa for Switch..."
/opt/devkitpro/meson-cross.sh switch switch_cross_file.txt build --reconfigure -Db_ndebug=true || /opt/devkitpro/meson-cross.sh switch switch_cross_file.txt build -Db_ndebug=true

echo "Fixing timespec_get multiple definition..."
sed -i "s/'-D__SWITCH__'/'-D__SWITCH__','-DHAVE_TIMESPEC_GET'/g" switch_cross_file.txt
meson setup build --reconfigure --cross-file switch_cross_file.txt -Db_ndebug=true

echo "Building Mesa..."
ninja -C build

echo "Installing Mesa..."
ninja -C build install

echo "Fixing egl.pc..."
sed -i "s,-lEGL,-lEGL -ldrm_nouveau," /opt/devkitpro/portlibs/switch/lib/pkgconfig/egl.pc

echo "Installing OpenGLConfig.cmake..."
install -Dm644 OpenGLConfig.cmake /opt/devkitpro/portlibs/switch/lib/cmake/OpenGL/OpenGLConfig.cmake
