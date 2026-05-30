# Whisplay U-Boot

Custom U-Boot build for [Whisplay HAT](https://github.com/PiSugar/Whisplay) that displays the boot logo on the SPI LCD during early boot, before the Linux kernel starts.

## Supported Hardware

- Raspberry Pi Zero 2W (BCM2837)

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

### Build

```bash
git clone https://github.com/PiSugar/whisplay-u-boot.git
cd whisplay-u-boot
bash build.sh
```

The output binary is at `output/u-boot-whisplay-zero2w.bin`.

### Cross-compile on another Pi (e.g. CM5)

```bash
# On aarch64 Pi, native compile works:
CROSS_COMPILE="" bash build.sh
```

## Deployment

1. Copy the binary and logo BMP to the boot partition:

```bash
sudo cp output/u-boot-whisplay-zero2w.bin /boot/firmware/
sudo cp logo_lcd_240_280_rgb565.bmp /boot/firmware/
```

2. Edit `/boot/firmware/config.txt`, add at the top:

```
kernel=u-boot-whisplay-zero2w.bin
```

3. Reboot. The logo should appear within ~500ms of power-on.

## Boot Logo BMP Requirements

- Format: 16-bit RGB565 BMP (Windows 3.x format, BI_BITFIELDS compression)
- Resolution: 240×280 pixels
- File name: `logo_lcd_240_280_rgb565.bmp`
- Location: root of the boot FAT partition (`/boot/firmware/`)

## How It Works

- Uses **direct BCM2835 SPI register access** (no U-Boot DM/DTS dependency) for maximum reliability
- SPI clock: 62.5 MHz (core_clk / 4)
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
│   └── whisplay_zero2w_defconfig  # U-Boot defconfig for Pi Zero 2W
├── dts-patches/
│   └── bcm2837-rpi-zero-2-w.dts  # Modified DTS (SPI enabled, no st7789v node)
└── output/                   # Build output (generated)
    └── u-boot-whisplay-zero2w.bin
```

## Reverting to Normal Boot

Remove or comment out the `kernel=` line in `/boot/firmware/config.txt`:

```
# kernel=u-boot-whisplay-zero2w.bin
```

## License

- `cmd_show_logo.c`: MIT (based on M5Stack code)
- U-Boot: GPL-2.0+
