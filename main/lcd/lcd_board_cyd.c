/* ESP32-2432S028R ("Cheap Yellow Display", 2.8 inch) board layer: ILI9341
 * (240x320, used landscape, SPI2/HSPI) and an XPT2046 resistive touch
 * controller on its own pins (SPI3/VSPI, polled). The panel reset is tied to
 * EN, so it gets a software reset. The board has no front buttons: touches
 * map to keys through lcd_view_key_at(). The RGB LED (active low) is kept
 * off. Two-port boards (CYD2USB) carry an ST7789 instead, selected with
 * CONFIG_ESP_SDR_LCD_CYD_ST7789; there is no reliable run-time detection. */
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_board.h"

#define PIN_LCD_SCLK 14
#define PIN_LCD_MOSI 13
#define PIN_LCD_MISO 12        /* wired to the panel, unused */
#define PIN_LCD_CS 15
#define PIN_LCD_DC 2
#define PIN_LCD_BL 21          /* active high */
#define PIN_TOUCH_SCLK 25
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39
#define PIN_TOUCH_CS 33
#define PIN_TOUCH_IRQ 36       /* low while touched */
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17
#define LCD_HOST SPI2_HOST
#define TOUCH_HOST SPI3_HOST
#define LCD_PCLK_HZ 40000000
#define TOUCH_CLK_HZ 2000000

/* XPT2046 12-bit conversions, differential, PENIRQ left enabled. */
#define XPT_X 0x90             /* along the long (landscape x) axis */
#define XPT_Y 0xd0
#define XPT_Z1 0xb0
#define XPT_Z2 0xc0
#define TOUCH_Z_MIN 400
/* Raw readings at the edges of the landscape panel (common CYD values). */
#define TOUCH_X_LEFT 200
#define TOUCH_X_RIGHT 3700
#define TOUCH_Y_TOP 240
#define TOUCH_Y_BOTTOM 3800

static esp_lcd_panel_io_handle_t io;
static spi_device_handle_t touch;
static lcd_board_sent_cb_t on_sent;

static bool color_sent(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    (void)panel_io; (void)e; (void)ctx;
    return on_sent();
}

typedef struct {
    uint8_t cmd, len, delay_ms;
    uint8_t data[15];
} panel_cmd_t;

#if CONFIG_ESP_SDR_LCD_CYD_ST7789
/* Software reset and wake, then the ILI9341 orientation; the ST7789 panel
 * is IPS and needs inverted colors. */
static const panel_cmd_t init_cmds[] = {
    {0x01, 0, 150, {0}},
    {0x11, 0, 120, {0}},
    {0x3a, 1, 0, {0x55}},
    {0x36, 1, 0, {0x28}},
    {0x21, 0, 0, {0}},
    {0x13, 0, 10, {0}},
    {0x29, 0, 20, {0}},
};
#else
/* Software reset, then esp_lcd_ili9341's default sequence. */
static const panel_cmd_t init_cmds[] = {
    {0x01, 0, 150, {0}},
    {0xcf, 3, 0, {0x00, 0xaa, 0xe0}},
    {0xed, 4, 0, {0x67, 0x03, 0x12, 0x81}},
    {0xe8, 3, 0, {0x8a, 0x01, 0x78}},
    {0xcb, 5, 0, {0x39, 0x2c, 0x00, 0x34, 0x02}},
    {0xf7, 1, 0, {0x20}},
    {0xea, 2, 0, {0x00, 0x00}},
    {0xc0, 1, 0, {0x23}},
    {0xc1, 1, 0, {0x11}},
    {0xc5, 2, 0, {0x43, 0x4c}},
    {0xc7, 1, 0, {0xa0}},
    {0xb1, 2, 0, {0x00, 0x1b}},
    {0xf2, 1, 0, {0x00}},
    {0x26, 1, 0, {0x01}},
    {0xe0, 15, 0, {0x1f, 0x36, 0x36, 0x3a, 0x0c, 0x05, 0x4f, 0x87, 0x3c, 0x08, 0x11, 0x35, 0x19, 0x13, 0x00}},
    {0xe1, 15, 0, {0x00, 0x09, 0x09, 0x05, 0x13, 0x0a, 0x30, 0x78, 0x43, 0x07, 0x0e, 0x0a, 0x26, 0x2c, 0x1f}},
    {0xb7, 1, 0, {0x07}},
    {0xb6, 3, 0, {0x08, 0x82, 0x27}},
    /* 16-bit pixels; row/column exchange and BGR for landscape; not
     * inverted (TN panel); then wake. */
    {0x3a, 1, 0, {0x55}},
    {0x36, 1, 0, {0x28}},
    {0x20, 0, 0, {0}},
    {0x11, 0, 120, {0}},
    {0x29, 0, 20, {0}},
};
#endif

void lcd_board_init(unsigned max_rows, lcd_board_sent_cb_t sent) {
    on_sent = sent;
    const gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_LCD_BL) | BIT64(PIN_LED_R) | BIT64(PIN_LED_G) | BIT64(PIN_LED_B),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(PIN_LED_R, 1);
    gpio_set_level(PIN_LED_G, 1);
    gpio_set_level(PIN_LED_B, 1);
    gpio_set_level(PIN_LCD_BL, 0);
    ESP_ERROR_CHECK(gpio_config(&out));

    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_BOARD_W * max_rows * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS, .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4,
        .on_color_trans_done = color_sent,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));
    for (unsigned i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        const panel_cmd_t *c = &init_cmds[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, c->cmd, c->len ? c->data : NULL, c->len));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    const spi_bus_config_t touch_bus = {
        .sclk_io_num = PIN_TOUCH_SCLK, .mosi_io_num = PIN_TOUCH_MOSI, .miso_io_num = PIN_TOUCH_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_HOST, &touch_bus, SPI_DMA_DISABLED));
    const spi_device_interface_config_t touch_dev = {
        .clock_speed_hz = TOUCH_CLK_HZ, .mode = 0, .spics_io_num = PIN_TOUCH_CS, .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(TOUCH_HOST, &touch_dev, &touch));
    const gpio_config_t irq = {.pin_bit_mask = BIT64(PIN_TOUCH_IRQ), .mode = GPIO_MODE_INPUT};
    ESP_ERROR_CHECK(gpio_config(&irq));
}

void lcd_board_backlight(bool on) { gpio_set_level(PIN_LCD_BL, on); }

void lcd_board_draw(int y0, int y1, const uint16_t *pixels) {
    const uint8_t cols[4] = {0, 0, (LCD_BOARD_W - 1) >> 8, (LCD_BOARD_W - 1) & 0xff};
    const uint8_t rows[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)((y1 - 1) >> 8), (uint8_t)(y1 - 1)};
    esp_lcd_panel_io_tx_param(io, 0x2a, cols, sizeof(cols));
    esp_lcd_panel_io_tx_param(io, 0x2b, rows, sizeof(rows));
    esp_lcd_panel_io_tx_color(io, 0x2c, pixels, (size_t)LCD_BOARD_W * (y1 - y0) * sizeof(uint16_t));
}

int lcd_board_battery(bool *charging) {
    *charging = false;
    return -1;
}

/* One conversion: the command byte, then 12 bits after a busy clock. */
static unsigned xpt_read(uint8_t cmd) {
    spi_transaction_t t = {.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA, .length = 24, .tx_data = {cmd}};
    if (spi_device_polling_transmit(touch, &t) != ESP_OK) return 0;
    return ((unsigned)t.rx_data[1] << 8 | t.rx_data[2]) >> 3 & 0xfffu;
}
/* The first of three conversions settles the drivers; it is often off. */
static unsigned xpt_median(uint8_t cmd) {
    unsigned a = xpt_read(cmd), b = xpt_read(cmd), c = xpt_read(cmd);
    return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
}
static unsigned xpt_pressure(void) { return xpt_read(XPT_Z1) + 4095u - xpt_read(XPT_Z2); }

static int scale(unsigned raw, int lo, int hi, int size) {
    int v = ((int)raw - lo) * size / (hi - lo);
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

lcd_key_t lcd_board_read_key(void) {
    if (xpt_pressure() < TOUCH_Z_MIN) return LCD_KEY_NONE;
    const unsigned x = xpt_median(XPT_X), y = xpt_median(XPT_Y);
    /* Lifted during the conversions: the coordinates are not valid. */
    if (xpt_pressure() < TOUCH_Z_MIN) return LCD_KEY_NONE;
    return lcd_view_key_at(scale(x, TOUCH_X_LEFT, TOUCH_X_RIGHT, LCD_BOARD_W),
                           scale(y, TOUCH_Y_TOP, TOUCH_Y_BOTTOM, LCD_BOARD_H));
}

size_t lcd_board_input_status(char *out, size_t size) {
    const unsigned z = xpt_pressure(), x = xpt_median(XPT_X), y = xpt_median(XPT_Y);
    return (size_t)snprintf(out, size, "LCDINPUT touch z %u x %u y %u irq %d key %d\n", z, x, y,
                            gpio_get_level(PIN_TOUCH_IRQ), (int)lcd_board_read_key());
}

uint64_t lcd_board_pins(void) {
    return BIT64(PIN_LCD_SCLK) | BIT64(PIN_LCD_MOSI) | BIT64(PIN_LCD_MISO) | BIT64(PIN_LCD_CS) |
           BIT64(PIN_LCD_DC) | BIT64(PIN_LCD_BL) | BIT64(PIN_TOUCH_SCLK) | BIT64(PIN_TOUCH_MOSI) |
           BIT64(PIN_TOUCH_MISO) | BIT64(PIN_TOUCH_CS) | BIT64(PIN_TOUCH_IRQ) | BIT64(PIN_LED_R) |
           BIT64(PIN_LED_G) | BIT64(PIN_LED_B);
}
