/*
 * Whisplay boot logo display for Raspberry Pi family.
 * Auto-detects SoC and uses direct register access for SPI + GPIO.
 *
 * Supported platforms:
 *   - BCM2837 (Pi 3, Zero 2W) - native BCM SPI + GPIO
 *   - BCM2711 (Pi 4, CM4)     - native BCM SPI + GPIO
 *   - BCM2712 (Pi 5, CM5)     - RP1 DW_apb_ssi SPI + RP1 GPIO via PCIe
 *
 * If no BMP file found, skips silently without affecting boot.
 *
 * SPDX-License-Identifier: MIT
 */

#include <bmp_layout.h>
#include <command.h>
#include <env.h>
#include <linux/delay.h>
#include <malloc.h>
#include <mapmem.h>
#include <asm/io.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

/* ===== Platform detection ===== */
enum whisplay_platform {
	PLATFORM_UNKNOWN = 0,
	PLATFORM_BCM2837,  /* Pi 3, Zero 2W */
	PLATFORM_BCM2711,  /* Pi 4, CM4 */
	PLATFORM_BCM2712,  /* Pi 5, CM5 (RP1) */
};

/* ===== BCM283x / BCM2711 native SPI registers ===== */
#define BCM_SPI_CS              0x00
#define BCM_SPI_FIFO            0x04
#define BCM_SPI_CLK             0x08
#define BCM_SPI_CS_TA           (1 << 7)
#define BCM_SPI_CS_DONE         (1 << 16)
#define BCM_SPI_CS_TXD          (1 << 18)
#define BCM_SPI_CS_RXD          (1 << 17)
#define BCM_SPI_CS_CLEAR_TX     (1 << 4)
#define BCM_SPI_CS_CLEAR_RX     (1 << 5)

/* BCM GPIO register offsets */
#define BCM_GPSET0              0x1C
#define BCM_GPCLR0              0x28

/* ===== RP1 (Pi 5/CM5) register definitions ===== */
#define RP1_BAR_BASE            0x1F00000000ULL
#define RP1_IO_BANK0_OFFSET     0x0D0000
#define RP1_SYS_RIO0_OFFSET     0x0E0000
#define RP1_PADS_BANK0_OFFSET   0x0F0000
#define RP1_SPI0_OFFSET         0x050000

/* RP1 GPIO CTRL register: FUNCSEL in bits [4:0] */
#define RP1_GPIO_CTRL(pin)      (8 * (pin) + 4)
#define RP1_FUNCSEL_SPI0        0   /* a0 = SPI0 */
#define RP1_FUNCSEL_SYS_RIO     5   /* a5 = SYS_RIO */
#define RP1_FUNCSEL_NULL        31

/* RP1 RIO: output register with atomic SET/CLR */
#define RP1_RIO_OUT             0x00
#define RP1_RIO_OE              0x04
#define RP1_RIO_SET_OFFSET      0x2000
#define RP1_RIO_CLR_OFFSET      0x3000

/* RP1 PADS: per-pin pad control (offset 0x04 + pin*4) */
#define RP1_PAD_GPIO(pin)       (0x04 + (pin) * 4)
#define RP1_PAD_OD              (1 << 7)  /* output disable */
#define RP1_PAD_IE              (1 << 6)  /* input enable */
#define RP1_PAD_DRIVE_8MA       (0x2 << 4)
#define RP1_PAD_SLEWFAST        (1 << 0)

/* RP1 DW_apb_ssi SPI registers */
#define DW_SPI_CTRLR0           0x00
#define DW_SPI_CTRLR1           0x04
#define DW_SPI_SSIENR           0x08
#define DW_SPI_SER              0x10
#define DW_SPI_BAUDR            0x14
#define DW_SPI_TXFTLR           0x18
#define DW_SPI_RXFTLR           0x1C
#define DW_SPI_TXFLR            0x20
#define DW_SPI_RXFLR            0x24
#define DW_SPI_SR               0x28
#define DW_SPI_DR               0x60

/* DW SPI Status Register bits */
#define DW_SPI_SR_BUSY          (1 << 0)
#define DW_SPI_SR_TFNF          (1 << 1)
#define DW_SPI_SR_TFE           (1 << 2)
#define DW_SPI_SR_RFNE          (1 << 3)

/* ===== Whisplay hardware definitions ===== */
#define WHISPLAY_GPIO_RST       4
#define WHISPLAY_GPIO_BL        22
#define WHISPLAY_GPIO_DC        27
#define WHISPLAY_LCD_WIDTH      240
#define WHISPLAY_LCD_HEIGHT     280
#define WHISPLAY_LCD_X_OFFSET   0
#define WHISPLAY_LCD_Y_OFFSET   20

/* ===== Global state ===== */
static enum whisplay_platform platform;
static void __iomem *gpio_base;
static void __iomem *spi_base;
static void __iomem *rio_base;   /* RP1 only */
static void __iomem *pads_base;  /* RP1 only */

/* ===== Platform detection ===== */
static enum whisplay_platform detect_platform(void)
{
	unsigned long midr;

	asm volatile("mrs %0, midr_el1" : "=r" (midr));
	unsigned int part = (midr >> 4) & 0xFFF;

	switch (part) {
	case 0xD03: return PLATFORM_BCM2837;
	case 0xD08: return PLATFORM_BCM2711;
	case 0xD4F: return PLATFORM_BCM2712; /* Cortex-A76 r4p1 */
	default:
		printf("[show_logo] Unknown SoC (MIDR part=0x%03x), skip\n", part);
		return PLATFORM_UNKNOWN;
	}
}

static unsigned long bcm_peri_base(void)
{
	switch (platform) {
	case PLATFORM_BCM2837: return 0x3F000000UL;
	case PLATFORM_BCM2711: return 0xFE000000UL;
	default: return 0;
	}
}

/* ===== BCM283x/2711 GPIO ===== */
static void bcm_gpio_set_func(int pin, int func)
{
	int reg = pin / 10;
	int shift = (pin % 10) * 3;
	uint32_t val = readl(gpio_base + reg * 4);
	val &= ~(7 << shift);
	val |= (func << shift);
	writel(val, gpio_base + reg * 4);
}

static void bcm_gpio_out(int pin, int val)
{
	if (val)
		writel(1 << pin, gpio_base + BCM_GPSET0);
	else
		writel(1 << pin, gpio_base + BCM_GPCLR0);
}

/* ===== RP1 GPIO ===== */
static void rp1_gpio_set_funcsel(int pin, int funcsel)
{
	writel(funcsel & 0x1F, gpio_base + RP1_GPIO_CTRL(pin));
}

static void rp1_gpio_set_output(int pin)
{
	/* Configure pad: enable output, 8mA drive, fast slew */
	uint32_t pad = RP1_PAD_IE | RP1_PAD_DRIVE_8MA | RP1_PAD_SLEWFAST;
	writel(pad, pads_base + RP1_PAD_GPIO(pin));

	/* Set function to SYS_RIO */
	rp1_gpio_set_funcsel(pin, RP1_FUNCSEL_SYS_RIO);

	/* Enable output in RIO */
	writel(1 << pin, rio_base + RP1_RIO_OE + RP1_RIO_SET_OFFSET);
}

static void rp1_gpio_out(int pin, int val)
{
	if (val)
		writel(1 << pin, rio_base + RP1_RIO_OUT + RP1_RIO_SET_OFFSET);
	else
		writel(1 << pin, rio_base + RP1_RIO_OUT + RP1_RIO_CLR_OFFSET);
}

/* ===== Unified GPIO abstraction ===== */
static void gpio_init_output(int pin)
{
	if (platform == PLATFORM_BCM2712) {
		rp1_gpio_set_output(pin);
	} else {
		bcm_gpio_set_func(pin, 1); /* output */
	}
}

static void gpio_set(int pin, int val)
{
	if (platform == PLATFORM_BCM2712)
		rp1_gpio_out(pin, val);
	else
		bcm_gpio_out(pin, val);
}

/* ===== BCM283x/2711 SPI ===== */
static void bcm_spi_init(void)
{
	bcm_gpio_set_func(8, 4);   /* CE0 - ALT0 */
	bcm_gpio_set_func(9, 4);   /* MISO - ALT0 */
	bcm_gpio_set_func(10, 4);  /* MOSI - ALT0 */
	bcm_gpio_set_func(11, 4);  /* SCLK - ALT0 */

	writel(4, spi_base + BCM_SPI_CLK);
	writel(BCM_SPI_CS_CLEAR_TX | BCM_SPI_CS_CLEAR_RX, spi_base + BCM_SPI_CS);
}

static void bcm_spi_transfer(const uint8_t *tx, size_t len)
{
	writel(BCM_SPI_CS_CLEAR_TX | BCM_SPI_CS_CLEAR_RX | BCM_SPI_CS_TA,
	       spi_base + BCM_SPI_CS);

	size_t tx_count = 0, rx_count = 0;
	while (rx_count < len) {
		uint32_t cs = readl(spi_base + BCM_SPI_CS);
		if ((cs & BCM_SPI_CS_TXD) && tx_count < len) {
			writel(tx[tx_count], spi_base + BCM_SPI_FIFO);
			tx_count++;
		}
		if (cs & BCM_SPI_CS_RXD) {
			(void)readl(spi_base + BCM_SPI_FIFO);
			rx_count++;
		}
	}
	while (!(readl(spi_base + BCM_SPI_CS) & BCM_SPI_CS_DONE))
		;
	writel(0, spi_base + BCM_SPI_CS);
}

/* ===== RP1 DW_apb_ssi SPI ===== */
static void rp1_spi_init(void)
{
	/* Set GPIO 8-11 to SPI0 function (a0) */
	rp1_gpio_set_funcsel(8, RP1_FUNCSEL_SPI0);
	rp1_gpio_set_funcsel(9, RP1_FUNCSEL_SPI0);
	rp1_gpio_set_funcsel(10, RP1_FUNCSEL_SPI0);
	rp1_gpio_set_funcsel(11, RP1_FUNCSEL_SPI0);

	/* Configure pads for SPI pins: enable output + input, 8mA, fast slew */
	for (int pin = 8; pin <= 11; pin++) {
		uint32_t pad = RP1_PAD_IE | RP1_PAD_DRIVE_8MA | RP1_PAD_SLEWFAST;
		writel(pad, pads_base + RP1_PAD_GPIO(pin));
	}

	/* Disable SSI to configure */
	writel(0, spi_base + DW_SPI_SSIENR);

	/*
	 * CTRLR0: 8-bit frame (DFS=7), SPI mode 0, TX&RX mode
	 * Bits [3:0]=DFS=7, [5:4]=FRF=0 (Motorola), [7:6]=SCPOL/SCPH=0,
	 * [9:8]=TMOD=0 (TX&RX)
	 */
	writel(0x0007, spi_base + DW_SPI_CTRLR0);

	/* Baud rate: clk_sys(200MHz) / 4 = 50MHz */
	writel(4, spi_base + DW_SPI_BAUDR);

	/* TX FIFO threshold */
	writel(0, spi_base + DW_SPI_TXFTLR);
	writel(0, spi_base + DW_SPI_RXFTLR);

	/* Enable slave 0 (CE0) */
	writel(1, spi_base + DW_SPI_SER);

	/* Enable SSI */
	writel(1, spi_base + DW_SPI_SSIENR);
}

static void rp1_spi_transfer(const uint8_t *tx, size_t len)
{
	/*
	 * DW_apb_ssi manages CS automatically while SSIENR=1 and SER is set.
	 * We disable/re-enable per transfer to get clean CS edges.
	 */
	writel(0, spi_base + DW_SPI_SSIENR);
	writel(1, spi_base + DW_SPI_SSIENR);

	size_t tx_idx = 0, rx_idx = 0;
	while (rx_idx < len) {
		uint32_t sr = readl(spi_base + DW_SPI_SR);
		if ((sr & DW_SPI_SR_TFNF) && tx_idx < len) {
			writel(tx[tx_idx], spi_base + DW_SPI_DR);
			tx_idx++;
		}
		if (sr & DW_SPI_SR_RFNE) {
			(void)readl(spi_base + DW_SPI_DR);
			rx_idx++;
		}
	}
	/* Wait for not busy */
	while (readl(spi_base + DW_SPI_SR) & DW_SPI_SR_BUSY)
		;
}

/* ===== Unified SPI abstraction ===== */
static void spi_hw_init(void)
{
	if (platform == PLATFORM_BCM2712)
		rp1_spi_init();
	else
		bcm_spi_init();
}

static void spi_xfer(const uint8_t *tx, size_t len)
{
	if (platform == PLATFORM_BCM2712)
		rp1_spi_transfer(tx, len);
	else
		bcm_spi_transfer(tx, len);
}

/* ===== LCD driver ===== */
static void lcd_cmd(uint8_t cmd)
{
	gpio_set(WHISPLAY_GPIO_DC, 0);
	spi_xfer(&cmd, 1);
}

static void lcd_data_buf(const uint8_t *data, size_t len)
{
	gpio_set(WHISPLAY_GPIO_DC, 1);
	spi_xfer(data, len);
}

static void lcd_data8(uint8_t val)
{
	lcd_data_buf(&val, 1);
}

static void lcd_reset(void)
{
	gpio_set(WHISPLAY_GPIO_RST, 1);
	mdelay(5);
	gpio_set(WHISPLAY_GPIO_RST, 0);
	mdelay(5);
	gpio_set(WHISPLAY_GPIO_RST, 1);
	mdelay(120);
}

static void lcd_init_seq(void)
{
	lcd_cmd(0x11);
	mdelay(120);

	lcd_cmd(0x36); lcd_data8(0xC0);
	lcd_cmd(0x3A); lcd_data8(0x05);

	lcd_cmd(0xB2);
	{ uint8_t d[] = {0x0C,0x0C,0x00,0x33,0x33}; lcd_data_buf(d, 5); }

	lcd_cmd(0xB7); lcd_data8(0x35);
	lcd_cmd(0xBB); lcd_data8(0x32);
	lcd_cmd(0xC2); lcd_data8(0x01);
	lcd_cmd(0xC3); lcd_data8(0x15);
	lcd_cmd(0xC4); lcd_data8(0x20);
	lcd_cmd(0xC6); lcd_data8(0x0F);

	lcd_cmd(0xD0);
	{ uint8_t d[] = {0xA4, 0xA1}; lcd_data_buf(d, 2); }

	lcd_cmd(0xE0);
	{ uint8_t d[] = {0xD0,0x08,0x0E,0x09,0x09,0x05,0x31,0x33,
	                 0x48,0x17,0x14,0x15,0x31,0x34}; lcd_data_buf(d, 14); }
	lcd_cmd(0xE1);
	{ uint8_t d[] = {0xD0,0x08,0x0E,0x09,0x09,0x15,0x31,0x33,
	                 0x48,0x17,0x14,0x15,0x31,0x34}; lcd_data_buf(d, 14); }

	lcd_cmd(0x21);
	lcd_cmd(0x29);
}

static void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	uint16_t xe = x + w - 1;
	uint16_t ye = y + h - 1;

	lcd_cmd(0x2A);
	{ uint8_t d[] = {x>>8, x&0xFF, xe>>8, xe&0xFF}; lcd_data_buf(d, 4); }
	lcd_cmd(0x2B);
	{ uint8_t d[] = {y>>8, y&0xFF, ye>>8, ye&0xFF}; lcd_data_buf(d, 4); }
	lcd_cmd(0x2C);
}

static void lcd_draw_pixels(const uint8_t *buf, uint32_t len)
{
	gpio_set(WHISPLAY_GPIO_DC, 1);
	while (len > 0) {
		uint32_t chunk = len > 4096 ? 4096 : len;
		spi_xfer(buf, chunk);
		buf += chunk;
		len -= chunk;
	}
}

/* ===== Hardware initialization ===== */
static int hw_init_peripherals(void)
{
	if (platform == PLATFORM_BCM2712) {
		gpio_base = map_sysmem(RP1_BAR_BASE + RP1_IO_BANK0_OFFSET, 0x200);
		rio_base = map_sysmem(RP1_BAR_BASE + RP1_SYS_RIO0_OFFSET, 0x4000);
		pads_base = map_sysmem(RP1_BAR_BASE + RP1_PADS_BANK0_OFFSET, 0x100);
		spi_base = map_sysmem(RP1_BAR_BASE + RP1_SPI0_OFFSET, 0x100);
		if (!gpio_base || !rio_base || !pads_base || !spi_base)
			return -1;
	} else {
		unsigned long base = bcm_peri_base();
		gpio_base = map_sysmem(base + 0x200000, 0x100);
		spi_base = map_sysmem(base + 0x204000, 0x20);
		if (!gpio_base || !spi_base)
			return -1;
	}
	return 0;
}

/* ===== Main command ===== */
int custom_show_logo(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	int result;

	platform = detect_platform();
	if (platform == PLATFORM_UNKNOWN)
		return CMD_RET_SUCCESS;

	/* Load BMP before touching any hardware */
	result = run_command(
		"fatload mmc 0:1 ${kernel_addr_r} /logo_lcd_240_280_rgb565.bmp",
		0);
	if (result != 0)
		return CMD_RET_SUCCESS;

	char *addr_str = env_get("kernel_addr_r");
	if (!addr_str)
		return CMD_RET_SUCCESS;

	unsigned long bmp_addr = simple_strtoul(addr_str, NULL, 16);
	struct bmp_image *bmp = (struct bmp_image *)map_sysmem(bmp_addr, 0);
	if (!bmp)
		return CMD_RET_SUCCESS;

	uint32_t bmp_width = le32_to_cpu(bmp->header.width);
	int32_t bmp_height = (int32_t)le32_to_cpu(bmp->header.height);
	uint16_t bpp = le16_to_cpu(bmp->header.bit_count);

	if (bpp != 16 || bmp_width != WHISPLAY_LCD_WIDTH ||
	    (bmp_height != (int32_t)WHISPLAY_LCD_HEIGHT &&
	     bmp_height != -(int32_t)WHISPLAY_LCD_HEIGHT))
		return CMD_RET_SUCCESS;

	uint32_t data_offset = le32_to_cpu(bmp->header.data_offset);
	uint16_t *pixels = (uint16_t *)(bmp_addr + data_offset);
	uint32_t pixel_count = WHISPLAY_LCD_WIDTH * WHISPLAY_LCD_HEIGHT;
	void *pixel_buf = NULL;

	if (bmp_height < 0) {
		pixel_buf = malloc(pixel_count * 2);
		if (!pixel_buf)
			return CMD_RET_SUCCESS;
		uint16_t *dst = (uint16_t *)pixel_buf;
		for (int row = 0; row < WHISPLAY_LCD_HEIGHT; row++) {
			uint16_t *src_row = pixels +
				(WHISPLAY_LCD_HEIGHT - 1 - row) * WHISPLAY_LCD_WIDTH;
			for (int col = 0; col < WHISPLAY_LCD_WIDTH; col++)
				dst[row * WHISPLAY_LCD_WIDTH + col] =
					__builtin_bswap16(src_row[col]);
		}
		pixels = (uint16_t *)pixel_buf;
	} else {
		for (uint32_t i = 0; i < pixel_count; i++)
			pixels[i] = __builtin_bswap16(pixels[i]);
	}

	/* Initialize hardware only after BMP is confirmed valid */
	if (hw_init_peripherals() < 0)
		goto out;

	gpio_init_output(WHISPLAY_GPIO_DC);
	gpio_init_output(WHISPLAY_GPIO_RST);
	gpio_init_output(WHISPLAY_GPIO_BL);
	gpio_set(WHISPLAY_GPIO_BL, 1);

	spi_hw_init();
	lcd_reset();
	lcd_init_seq();

	lcd_set_window(WHISPLAY_LCD_X_OFFSET, WHISPLAY_LCD_Y_OFFSET,
		       WHISPLAY_LCD_WIDTH, WHISPLAY_LCD_HEIGHT);
	lcd_draw_pixels((const uint8_t *)pixels, pixel_count * 2);

	gpio_set(WHISPLAY_GPIO_BL, 0);

out:
	if (pixel_buf)
		free(pixel_buf);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(show_logo, 1, 1, custom_show_logo, "Show Whisplay boot logo",
	   "Display logo on SPI LCD; auto-detects Pi model, skips if unsupported or no BMP");
