/*
 * Whisplay boot logo via direct BCM2835 SPI register access.
 * If no Whisplay HAT or no BMP file, skips silently without affecting boot.
 */

#include <bmp_layout.h>
#include <command.h>
#include <env.h>
#include <linux/delay.h>
#include <malloc.h>
#include <mapmem.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

#define BCM2837_PERI_BASE   0x3F000000
#define GPIO_BASE           (BCM2837_PERI_BASE + 0x200000)
#define SPI0_BASE           (BCM2837_PERI_BASE + 0x204000)

#define SPI_CS              0x00
#define SPI_FIFO            0x04
#define SPI_CLK             0x08

#define SPI_CS_TA           (1 << 7)
#define SPI_CS_DONE         (1 << 16)
#define SPI_CS_TXD          (1 << 18)
#define SPI_CS_RXD          (1 << 17)
#define SPI_CS_CLEAR_TX     (1 << 4)
#define SPI_CS_CLEAR_RX     (1 << 5)

#define GPSET0              0x1C
#define GPCLR0              0x28

#define WHISPLAY_GPIO_RST   4
#define WHISPLAY_GPIO_BL    22
#define WHISPLAY_GPIO_DC    27
#define WHISPLAY_LCD_WIDTH  240
#define WHISPLAY_LCD_HEIGHT 280
#define WHISPLAY_LCD_X_OFFSET 0
#define WHISPLAY_LCD_Y_OFFSET 20

static void __iomem *gpio_base;
static void __iomem *spi_base;

static void gpio_set_func(int pin, int func)
{
	int reg = pin / 10;
	int shift = (pin % 10) * 3;
	uint32_t val = readl(gpio_base + reg * 4);
	val &= ~(7 << shift);
	val |= (func << shift);
	writel(val, gpio_base + reg * 4);
}

static void gpio_out(int pin, int val)
{
	if (val)
		writel(1 << pin, gpio_base + GPSET0);
	else
		writel(1 << pin, gpio_base + GPCLR0);
}

static void spi_init_hw(void)
{
	gpio_set_func(8, 4);   /* CE0 - ALT0 */
	gpio_set_func(9, 4);   /* MISO - ALT0 */
	gpio_set_func(10, 4);  /* MOSI - ALT0 */
	gpio_set_func(11, 4);  /* SCLK - ALT0 */

	writel(4, spi_base + SPI_CLK); /* 250MHz / 4 = 62.5MHz */
	writel(SPI_CS_CLEAR_TX | SPI_CS_CLEAR_RX, spi_base + SPI_CS);
}

static void spi_transfer(const uint8_t *tx, size_t len)
{
	writel(SPI_CS_CLEAR_TX | SPI_CS_CLEAR_RX | SPI_CS_TA,
	       spi_base + SPI_CS);

	size_t tx_count = 0, rx_count = 0;
	while (rx_count < len) {
		uint32_t cs = readl(spi_base + SPI_CS);
		if ((cs & SPI_CS_TXD) && tx_count < len) {
			writel(tx[tx_count], spi_base + SPI_FIFO);
			tx_count++;
		}
		if (cs & SPI_CS_RXD) {
			(void)readl(spi_base + SPI_FIFO);
			rx_count++;
		}
	}
	while (!(readl(spi_base + SPI_CS) & SPI_CS_DONE))
		;
	writel(0, spi_base + SPI_CS);
}

static void lcd_cmd(uint8_t cmd)
{
	gpio_out(WHISPLAY_GPIO_DC, 0);
	spi_transfer(&cmd, 1);
}

static void lcd_data_buf(const uint8_t *data, size_t len)
{
	gpio_out(WHISPLAY_GPIO_DC, 1);
	spi_transfer(data, len);
}

static void lcd_data8(uint8_t val)
{
	lcd_data_buf(&val, 1);
}

static void lcd_reset(void)
{
	gpio_out(WHISPLAY_GPIO_RST, 1);
	mdelay(5);
	gpio_out(WHISPLAY_GPIO_RST, 0);
	mdelay(5);
	gpio_out(WHISPLAY_GPIO_RST, 1);
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
	gpio_out(WHISPLAY_GPIO_DC, 1);
	while (len > 0) {
		uint32_t chunk = len > 4096 ? 4096 : len;
		spi_transfer(buf, chunk);
		buf += chunk;
		len -= chunk;
	}
}

int custom_show_logo(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	int result;

	gpio_base = map_sysmem(GPIO_BASE, 0x100);
	spi_base = map_sysmem(SPI0_BASE, 0x20);
	if (!gpio_base || !spi_base)
		return CMD_RET_SUCCESS;

	/*
	 * Try loading BMP first (before any HW init).
	 * If the file doesn't exist, skip entirely — no GPIO/SPI touched.
	 */
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

	/* Only touch hardware after BMP is confirmed valid */
	gpio_set_func(WHISPLAY_GPIO_DC, 1);
	gpio_set_func(WHISPLAY_GPIO_RST, 1);
	gpio_set_func(WHISPLAY_GPIO_BL, 1);
	gpio_out(WHISPLAY_GPIO_BL, 1);

	spi_init_hw();
	lcd_reset();
	lcd_init_seq();

	lcd_set_window(WHISPLAY_LCD_X_OFFSET, WHISPLAY_LCD_Y_OFFSET,
		       WHISPLAY_LCD_WIDTH, WHISPLAY_LCD_HEIGHT);
	lcd_draw_pixels((const uint8_t *)pixels, pixel_count * 2);

	gpio_out(WHISPLAY_GPIO_BL, 0);

	if (pixel_buf)
		free(pixel_buf);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(show_logo, 1, 1, custom_show_logo, "Show Whisplay boot logo",
	   "Display logo if BMP exists; skip silently otherwise");
