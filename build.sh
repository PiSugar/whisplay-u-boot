#!/usr/bin/env bash
set -euo pipefail

UBOOT_VERSION="${UBOOT_VERSION:-v2024.04}"
UBOOT_REPO="${UBOOT_REPO:-https://github.com/u-boot/u-boot.git}"
CROSS_COMPILE="${CROSS_COMPILE-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/u-boot"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# Default: generic build for all Pi models (3/Zero2W/4/CM4/5/CM5)
DEFCONFIG="${DEFCONFIG:-whisplay_rpi_arm64_defconfig}"

echo "=== Whisplay U-Boot Build ==="
echo "U-Boot repo: ${UBOOT_REPO}"
echo "U-Boot: ${UBOOT_VERSION}"
echo "Defconfig: ${DEFCONFIG}"
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
    echo "U-Boot source already present at ${BUILD_DIR}"
fi

echo "Applying Whisplay patches..."

cp "${SCRIPT_DIR}/cmd/cmd_show_logo.c" "${BUILD_DIR}/cmd/"

cp "${SCRIPT_DIR}/configs/"*.defconfig "${BUILD_DIR}/configs/" 2>/dev/null || true
cp "${SCRIPT_DIR}/configs/"*_defconfig "${BUILD_DIR}/configs/" 2>/dev/null || true

if [ -f "${SCRIPT_DIR}/dts-patches/bcm2837-rpi-zero-2-w.dts" ] && \
   [ -d "${BUILD_DIR}/dts/upstream/src/arm/broadcom" ]; then
    cp "${SCRIPT_DIR}/dts-patches/bcm2837-rpi-zero-2-w.dts" \
       "${BUILD_DIR}/dts/upstream/src/arm/broadcom/bcm2837-rpi-zero-2-w.dts"
fi

if ! grep -q 'cmd_show_logo' "${BUILD_DIR}/cmd/Makefile"; then
    echo 'obj-y += cmd_show_logo.o' >> "${BUILD_DIR}/cmd/Makefile"
fi

if [ -d "${SCRIPT_DIR}/patches" ]; then
    for p in "${SCRIPT_DIR}/patches/"*.patch; do
        [ -f "$p" ] || continue
        echo "Applying $(basename "$p")..."
        if patch -d "${BUILD_DIR}" -p1 --forward --dry-run --batch \
            < "$p" >/dev/null 2>&1; then
            patch -d "${BUILD_DIR}" -p1 --forward --batch < "$p"
        else
            echo "  already applied or not applicable, skipping"
        fi
    done
fi

echo "Configuring (${DEFCONFIG})..."
cd "${BUILD_DIR}"
make "${DEFCONFIG}" CROSS_COMPILE="${CROSS_COMPILE}"

echo "Building (j${JOBS})..."
make -j"${JOBS}" CROSS_COMPILE="${CROSS_COMPILE}"

mkdir -p "${OUTPUT_DIR}"

OUTPUT_NAME="u-boot-whisplay-rpi-arm64.bin"
if [[ "${DEFCONFIG}" == *"zero2w"* ]]; then
    OUTPUT_NAME="u-boot-whisplay-zero2w.bin"
fi

cp "${BUILD_DIR}/u-boot.bin" "${OUTPUT_DIR}/${OUTPUT_NAME}"

echo ""
echo "=== Build complete ==="
echo "Output: ${OUTPUT_DIR}/${OUTPUT_NAME}"
echo ""
echo "Deploy to Pi:"
echo "  sudo cp ${OUTPUT_DIR}/${OUTPUT_NAME} /boot/firmware/"
echo "  sudo cp logo_lcd_240_280_rgb565.bmp /boot/firmware/"
echo "  Add to /boot/firmware/config.txt:"
echo "    enable_uart=1"
echo "    uart_2ndstage=1"
echo "    kernel=${OUTPUT_NAME}"
