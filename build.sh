#!/usr/bin/env bash
set -euo pipefail

UBOOT_VERSION="v2026.07-rc2"
UBOOT_REPO="https://source.denx.de/u-boot/u-boot.git"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/u-boot"
OUTPUT_DIR="${SCRIPT_DIR}/output"

echo "=== Whisplay U-Boot Build ==="
echo "Target: Raspberry Pi Zero 2W"
echo "U-Boot version: ${UBOOT_VERSION}"
echo "Cross compiler: ${CROSS_COMPILE}gcc"
echo ""

if ! command -v "${CROSS_COMPILE}gcc" &>/dev/null; then
    echo "Error: ${CROSS_COMPILE}gcc not found."
    echo "Install: sudo apt-get install gcc-aarch64-linux-gnu"
    exit 1
fi

if [ ! -d "${BUILD_DIR}" ]; then
    echo "Cloning U-Boot ${UBOOT_VERSION}..."
    git clone --depth 1 --branch "${UBOOT_VERSION}" "${UBOOT_REPO}" "${BUILD_DIR}"
else
    echo "U-Boot source already exists at ${BUILD_DIR}"
fi

echo "Applying Whisplay patches..."

cp "${SCRIPT_DIR}/cmd/cmd_show_logo.c" "${BUILD_DIR}/cmd/"

cp "${SCRIPT_DIR}/configs/whisplay_zero2w_defconfig" "${BUILD_DIR}/configs/"

cp "${SCRIPT_DIR}/dts-patches/bcm2837-rpi-zero-2-w.dts" \
   "${BUILD_DIR}/dts/upstream/src/arm/broadcom/bcm2837-rpi-zero-2-w.dts"

if ! grep -q 'cmd_show_logo' "${BUILD_DIR}/cmd/Makefile"; then
    echo 'obj-y += cmd_show_logo.o' >> "${BUILD_DIR}/cmd/Makefile"
fi

echo "Configuring..."
cd "${BUILD_DIR}"
make whisplay_zero2w_defconfig CROSS_COMPILE="${CROSS_COMPILE}"

echo "Building (j${JOBS})..."
make -j"${JOBS}" CROSS_COMPILE="${CROSS_COMPILE}"

mkdir -p "${OUTPUT_DIR}"
cp "${BUILD_DIR}/u-boot.bin" "${OUTPUT_DIR}/u-boot-whisplay-zero2w.bin"

echo ""
echo "=== Build complete ==="
echo "Output: ${OUTPUT_DIR}/u-boot-whisplay-zero2w.bin"
echo ""
echo "Deploy to Pi:"
echo "  scp ${OUTPUT_DIR}/u-boot-whisplay-zero2w.bin pi@<IP>:/boot/firmware/"
echo "  Add to /boot/firmware/config.txt:"
echo "    kernel=u-boot-whisplay-zero2w.bin"
