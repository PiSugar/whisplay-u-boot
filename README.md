# Whisplay U-Boot

Custom U-Boot build for [Whisplay HAT](https://github.com/PiSugar/Whisplay) that displays the boot logo on the SPI LCD during early boot, before the Linux kernel starts.

## Supported Hardware

- Raspberry Pi Zero 2W (BCM2837)
- Raspberry Pi 3 Model B/B+ (BCM2837)
- Raspberry Pi 4 Model B (BCM2711)
- Raspberry Pi CM4 (BCM2711)
- Raspberry Pi 5 (BCM2712 + RP1)
- Raspberry Pi CM5 (BCM2712 + RP1)

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

### Build (generic, supports Pi 3/Zero2W/4/CM4)

```bash
git clone https://github.com/PiSugar/whisplay-u-boot.git
cd whisplay-u-boot
bash build.sh
```

The output binary is at `output/u-boot-whisplay-rpi-arm64.bin`.

### Build for a specific board (Pi Zero 2W only, with embedded DTB)

```bash
DEFCONFIG=whisplay_zero2w_defconfig bash build.sh
```

### Native compile on aarch64 Pi

```bash
CROSS_COMPILE="" bash build.sh
```

## Deployment

1. Copy the binary and logo BMP to the boot partition:

```bash
sudo cp output/u-boot-whisplay-rpi-arm64.bin /boot/firmware/
sudo cp logo_lcd_240_280_rgb565.bmp /boot/firmware/
```

2. Edit `/boot/firmware/config.txt`, add at the top:

```
kernel=u-boot-whisplay-rpi-arm64.bin
```

3. Reboot. The logo should appear within ~500ms of power-on.

## Boot Logo BMP Requirements

- Format: 16-bit RGB565 BMP (Windows 3.x format, BI_BITFIELDS compression)
- Resolution: 240×280 pixels
- File name: `logo_lcd_240_280_rgb565.bmp`
- Location: root of the boot FAT partition (`/boot/firmware/`)

## How It Works

- Uses **direct SPI register access** (no U-Boot DM/DTS dependency) for maximum reliability
- Auto-detects SoC via ARM MIDR register:
  - Cortex-A53 → BCM2837 (peripheral base `0x3F000000`)
  - Cortex-A72 → BCM2711 (peripheral base `0xFE000000`)
  - Cortex-A76 → BCM2712 (RP1 via PCIe BAR at `0x1F00000000`)
- SPI clock: core_clk / 4 (≈62.5 MHz on Pi 3/Zero2W, ≈125 MHz on Pi 4, ≈50 MHz on Pi 5)
- Pi 5/CM5: uses RP1 DesignWare DW_apb_ssi SPI + RIO GPIO via PCIe-mapped registers
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
│   └── cmd_show_logo.c      # U-Boot show_logo command (BCM2837/2711/2712+RP1)
├── configs/
│   ├── whisplay_rpi_arm64_defconfig  # Generic (Pi 3/Zero2W/4/CM4/5/CM5)
│   └── whisplay_zero2w_defconfig     # Pi Zero 2W specific (embedded DTB)
├── dts-patches/
│   └── bcm2837-rpi-zero-2-w.dts  # Modified DTS (for zero2w_defconfig only)
└── output/                   # Build output (generated)
    └── u-boot-whisplay-rpi-arm64.bin
```

## Reverting to Normal Boot

Remove or comment out the `kernel=` line in `/boot/firmware/config.txt`:

```
# kernel=u-boot-whisplay-zero2w.bin
```

## License

- `cmd_show_logo.c`: MIT (based on M5Stack code)
- U-Boot: GPL-2.0+
