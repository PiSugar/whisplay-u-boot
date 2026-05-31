#!/usr/bin/env bash
set -euo pipefail

REPO="${REPO:-PiSugar/whisplay-u-boot}"
VERSION="${VERSION:-latest}"
ASSET="${ASSET:-u-boot-whisplay-rpi-arm64.bin}"
BOOT_DIR="${BOOT_DIR:-}"

usage() {
    cat <<EOF
Usage:
  bash install.sh
  VERSION=v0.1.0 bash install.sh
  BIN_URL=https://github.com/PiSugar/whisplay-u-boot/releases/download/v0.1.0/u-boot-whisplay-rpi-arm64.bin bash install.sh

Environment:
  VERSION   GitHub release tag to install. Defaults to latest.
  BIN_URL   Override the binary download URL.
  BOOT_DIR  Override boot partition mount point. Defaults to /boot/firmware,
            falling back to /boot.

This installer only supports 64-bit Raspberry Pi Linux systems.
EOF
}

die() {
    echo "Error: $*" >&2
    exit 1
}

info() {
    echo "==> $*"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    die "this installer must run on Raspberry Pi Linux"
fi

arch="$(uname -m)"
long_bit="$(getconf LONG_BIT 2>/dev/null || echo unknown)"
if [[ "${arch}" != "aarch64" && "${arch}" != "arm64" ]] || [[ "${long_bit}" != "64" ]]; then
    die "64-bit Raspberry Pi OS is required (detected arch=${arch}, LONG_BIT=${long_bit})"
fi

model="$(tr -d '\0' </proc/device-tree/model 2>/dev/null || true)"
if [[ "${model}" != Raspberry\ Pi* ]]; then
    die "this does not look like a Raspberry Pi (model='${model:-unknown}')"
fi

if [[ -z "${BOOT_DIR}" ]]; then
    if [[ -d /boot/firmware ]]; then
        BOOT_DIR="/boot/firmware"
    elif [[ -d /boot ]]; then
        BOOT_DIR="/boot"
    else
        die "boot partition not found; set BOOT_DIR explicitly"
    fi
fi

config_txt="${BOOT_DIR}/config.txt"
[[ -f "${config_txt}" ]] || die "${config_txt} not found"
[[ -w "${BOOT_DIR}" && -w "${config_txt}" ]] || SUDO="${SUDO:-sudo}"
SUDO="${SUDO:-}"

if command -v curl >/dev/null 2>&1; then
    download() { curl -fL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    download() { wget -O "$2" "$1"; }
else
    die "curl or wget is required"
fi

download_with_fallback() {
    local url="$1"
    local dest="$2"
    local fallback_url

    if download "${url}" "${dest}"; then
        return 0
    fi

    if [[ "${url}" == *github.com* ]]; then
        fallback_url="${url/github.com/repo.pisugar.uk}"
        info "Download failed; retrying ${fallback_url}"
        download "${fallback_url}" "${dest}"
    else
        return 1
    fi
}

if [[ -n "${BIN_URL:-}" ]]; then
    bin_url="${BIN_URL}"
elif [[ "${VERSION}" == "latest" ]]; then
    bin_url="https://github.com/${REPO}/releases/download/latest/${ASSET}"
else
    bin_url="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT
tmp_bin="${tmp_dir}/${ASSET}"

info "Detected ${model}"
info "Using boot partition ${BOOT_DIR}"
info "Downloading ${bin_url}"
download_with_fallback "${bin_url}" "${tmp_bin}"

if [[ ! -s "${tmp_bin}" ]]; then
    die "downloaded binary is empty"
fi

ts="$(date +%Y%m%d%H%M%S)"
target_bin="${BOOT_DIR}/${ASSET}"

info "Backing up boot files"
${SUDO} cp "${config_txt}" "${config_txt}.whisplay-${ts}.bak"
if [[ -f "${target_bin}" ]]; then
    ${SUDO} cp "${target_bin}" "${target_bin}.whisplay-${ts}.bak"
fi

info "Installing ${ASSET}"
${SUDO} cp "${tmp_bin}" "${target_bin}"
${SUDO} chmod 0644 "${target_bin}"

set_config() {
    local key="$1"
    local value="$2"
    local escaped

    escaped="$(printf '%s' "${value}" | sed 's/[\/&]/\\&/g')"
    if grep -qE "^[[:space:]]*#?[[:space:]]*${key}=" "${config_txt}"; then
        ${SUDO} sed -i "s/^[[:space:]]*#\\?[[:space:]]*${key}=.*/${key}=${escaped}/" "${config_txt}"
    else
        printf '%s=%s\n' "${key}" "${value}" | ${SUDO} tee -a "${config_txt}" >/dev/null
    fi
}

info "Updating ${config_txt}"
set_config "enable_uart" "1"
set_config "uart_2ndstage" "1"
set_config "kernel" "${ASSET}"

info "Installed successfully"
sha256sum "${target_bin}" || true
cat <<EOF

Next steps:
  1. Put logo_lcd_240_280_rgb565.bmp in ${BOOT_DIR} if you want the boot logo.
  2. Reboot:
       sudo reboot

To revert, restore:
  ${config_txt}.whisplay-${ts}.bak
EOF
