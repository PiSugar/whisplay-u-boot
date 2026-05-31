#!/usr/bin/env python3
import subprocess
import time

import spidev


GPIO_RST = 4
GPIO_BL = 22
GPIO_LED_R = 23
GPIO_LED_B = 24
GPIO_LED_G = 25
GPIO_DC = 27

WIDTH = 240
HEIGHT = 280
Y_OFFSET = 20


def pin(pin, level):
    subprocess.run(
        ["pinctrl", "set", str(pin), "op", "dh" if level else "dl"],
        check=True,
    )


def cmd(spi, value):
    pin(GPIO_DC, 0)
    spi.xfer2([value])


def data(spi, values):
    pin(GPIO_DC, 1)
    spi.xfer2(values)


def reset_lcd():
    pin(GPIO_RST, 1)
    time.sleep(0.005)
    pin(GPIO_RST, 0)
    time.sleep(0.005)
    pin(GPIO_RST, 1)
    time.sleep(0.12)


def init_lcd(spi):
    cmd(spi, 0x11)
    time.sleep(0.12)
    cmd(spi, 0x36)
    data(spi, [0xC0])
    cmd(spi, 0x3A)
    data(spi, [0x05])
    cmd(spi, 0xB2)
    data(spi, [0x0C, 0x0C, 0x00, 0x33, 0x33])
    cmd(spi, 0xB7)
    data(spi, [0x35])
    cmd(spi, 0xBB)
    data(spi, [0x32])
    cmd(spi, 0xC2)
    data(spi, [0x01])
    cmd(spi, 0xC3)
    data(spi, [0x15])
    cmd(spi, 0xC4)
    data(spi, [0x20])
    cmd(spi, 0xC6)
    data(spi, [0x0F])
    cmd(spi, 0xD0)
    data(spi, [0xA4, 0xA1])
    cmd(spi, 0x21)
    cmd(spi, 0x29)


def window(spi):
    x0 = 0
    x1 = WIDTH - 1
    y0 = Y_OFFSET
    y1 = Y_OFFSET + HEIGHT - 1
    cmd(spi, 0x2A)
    data(spi, [x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF])
    cmd(spi, 0x2B)
    data(spi, [y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF])
    cmd(spi, 0x2C)


def fill(spi, hi, lo):
    pin(GPIO_DC, 1)
    chunk = [hi, lo] * 1024
    total = WIDTH * HEIGHT
    while total:
        n = min(total, 1024)
        spi.xfer2(chunk[: n * 2])
        total -= n


def set_led(r, g, b):
    pin(GPIO_LED_R, r)
    pin(GPIO_LED_G, g)
    pin(GPIO_LED_B, b)


def main():
    for p in [GPIO_RST, GPIO_BL, GPIO_LED_R, GPIO_LED_G, GPIO_LED_B, GPIO_DC]:
        pin(p, 0)

    spi = spidev.SpiDev()
    spi.open(0, 0)
    spi.mode = 0
    spi.max_speed_hz = 20000000

    for bl in [0, 1]:
        print(f"Backlight level {bl}")
        pin(GPIO_BL, bl)
        reset_lcd()
        init_lcd(spi)
        window(spi)
        for name, rgb565, led in [
            ("red", (0xF8, 0x00), (1, 0, 0)),
            ("green", (0x07, 0xE0), (0, 1, 0)),
            ("blue", (0x00, 0x1F), (0, 0, 1)),
        ]:
            print(name)
            set_led(*led)
            fill(spi, *rgb565)
            time.sleep(2)

    spi.close()
    set_led(0, 0, 0)
    print("done")


if __name__ == "__main__":
    main()
