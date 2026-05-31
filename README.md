# Whisplay U-Boot

Custom U-Boot build for [Whisplay HAT](https://github.com/PiSugar/Whisplay) that displays the boot logo on the SPI LCD during early boot, before the Linux kernel starts.

## Supported Hardware

- Raspberry Pi Zero 2W (BCM2837)
- Raspberry Pi 3 Model B/B+ (BCM2837)
- Raspberry Pi 4 Model B (BCM2711)
- Raspberry Pi CM4 (BCM2711)
- Raspberry Pi 5 (BCM2712)
- Raspberry Pi CM5 (BCM2712)

## What It Does

1. Pi firmware loads U-Boot instead of the kernel directly
2. U-Boot displays `logo_lcd_240_280_rgb565.bmp` from the boot partition on the Whisplay SPI LCD (ST7789, 240×280)
3. U-Boot then boots the Linux kernel normally

If the BMP file is not present on the boot partition, U-Boot skips logo display entirely — no GPIO or SPI pins are touched, and boot proceeds normally.

## Building

### Prerequisites

```bash
sudo apt-get install gcc-aarch64-linux-gnu make git bison flex libssl-dev
```

### Build (generic, supports all Pi models)

```bash
git clone https://github.com/PiSugar/whisplay-u-boot.git
cd whisplay-u-boot
bash build.sh
```

The output binary is at `output/u-boot-whisplay-rpi-arm64.bin`.

### Native compile on aarch64 Pi

```bash
CROSS_COMPILE="" bash build.sh
```

## Deployment

### Install from a Raspberry Pi

On a 64-bit Raspberry Pi OS system:

```bash
curl -fsSL https://raw.githubusercontent.com/PiSugar/whisplay-u-boot/main/install.sh | bash
```

Install a specific release:

```bash
curl -fsSL https://raw.githubusercontent.com/PiSugar/whisplay-u-boot/main/install.sh | VERSION=v0.1.0 bash
```

The installer only runs on 64-bit Raspberry Pi Linux. It downloads
`u-boot-whisplay-rpi-arm64.bin`, backs up `/boot/firmware/config.txt`, installs
the binary, and sets `enable_uart=1`, `uart_2ndstage=1`, and
`kernel=u-boot-whisplay-rpi-arm64.bin`.

### Manual Install

1. Copy the binary and logo BMP to the boot partition:

```bash
sudo cp output/u-boot-whisplay-rpi-arm64.bin /boot/firmware/
sudo cp logo_lcd_240_280_rgb565.bmp /boot/firmware/
```

2. Edit `/boot/firmware/config.txt`, add:

```ini
enable_uart=1
uart_2ndstage=1
kernel=u-boot-whisplay-rpi-arm64.bin
```

3. Reboot. The logo should appear within ~500ms of power-on (Pi 3/4/Zero2W).

## Boot Logo BMP Requirements

- Format: 16-bit RGB565 BMP (Windows 3.x format, BI_BITFIELDS compression)
- Resolution: 240×280 pixels
- File name: `logo_lcd_240_280_rgb565.bmp`
- Location: root of the boot FAT partition (`/boot/firmware/`)

## How It Works

- Based on **U-Boot v2024.04** (first version with confirmed Pi 5 support)
- Uses **direct SPI register access** (no U-Boot DM/DTS dependency) for maximum reliability
- Auto-detects SoC via ARM MIDR register:
  - Cortex-A53 (0xD03) → BCM2837 (peripheral base `0x3F000000`)
  - Cortex-A72 (0xD08) → BCM2711 (peripheral base `0xFE000000`)
  - Cortex-A76 (0xD0B) → BCM2712 (RP1 GPIO + DW_apb_ssi SPI)
- SPI clock:
  - BCM2837/BCM2711: core_clk / 4 (≈62.5 MHz on Pi 3/Zero2W, ≈125 MHz on Pi 4)
  - BCM2712: RP1 SPI0 at ≈20 MHz
- Custom BOOTCOMMAND handles both Pi 5 (`kernel_2712.img`) and Pi 3/4 (`kernel8.img`)
- `CONFIG_BOOTDELAY=-2` prevents hang on Pi 5 (UART RX floating issue)
- `kernel_comp_addr_r`/`kernel_comp_size` set for gzip-compressed kernel images
- Does **not** set `bootargs` — preserves firmware DTB cmdline (no hardcoded PARTUUID)
- GPIO pin mapping (BCM numbering):
  - DC: GPIO 27
  - RST: GPIO 4
  - BL: GPIO 22
  - SPI0: CE0=GPIO8, MOSI=GPIO10, SCLK=GPIO11

## File Structure

```
├── build.sh                  # Automated build script
├── cmd/
│   └── cmd_show_logo.c      # U-Boot show_logo command
├── configs/
│   └── whisplay_rpi_arm64_defconfig  # Unified Pi 3/Zero2W/4/CM4/5/CM5 build
└── output/                   # Build output (generated)
    └── u-boot-whisplay-rpi-arm64.bin
```

## Reverting to Normal Boot

Remove or comment out the `kernel=` and `uart_2ndstage=` lines in `/boot/firmware/config.txt`:

```
# kernel=u-boot-whisplay-rpi-arm64.bin
# uart_2ndstage=1
```

## Known Issues

- **Pi 5/CM5**: Boot takes ~10s longer due to `pci enum` timeout in PREBOOT.

## License

- `cmd_show_logo.c`: GPL-2.0+
- U-Boot: GPL-2.0+
