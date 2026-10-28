/* Pocket-Dongle-S3 board layer: ST7735S (0.96 inch IPS, 80x160, used
 * landscape, SPI2) and the BOOT button (GPIO0, the only button). The
 * backlight is wired on (LEDK through 10 ohms to GND), so the display is
 * switched with DISPOFF/DISPON instead. Pins follow the vendor schematic
 * ESP32-S3-0.96LCD. */
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_board.h"

#define PIN_LCD_SCLK 10
#define PIN_LCD_MOSI 11
#define PIN_LCD_CS 12
#define PIN_LCD_DC 13
#define PIN_LCD_RST 14
#define PIN_BUTTON 0           /* BOOT, active low, 10k pull-up */
#define LCD_HOST SPI2_HOST
#define LCD_PCLK_HZ 27000000
/* The 80x160 glass sits at column 26, row 1 of the 132x162 controller RAM;
 * the offsets are symmetric, so mirroring does not move them. Landscape
 * swaps them. */
#define LCD_X_OFFSET 1
#define LCD_Y_OFFSET 26

static esp_lcd_panel_io_handle_t io;
static lcd_board_sent_cb_t on_sent;

static bool color_sent(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    (void)panel_io; (void)e; (void)ctx;
    return on_sent();
}

typedef struct {
    uint8_t cmd, len, delay_ms;
    uint8_t data[16];
} panel_cmd_t;

/* Software reset and wake, the common ST7735S frame rate, power and gamma
 * settings, then 16-bit pixels, landscape (MX|MV) with BGR order, and
 * inversion for the IPS glass. The display stays off until backlight-on. */
static const panel_cmd_t init_cmds[] = {
    {0x01, 0, 150, {0}},
    {0x11, 0, 255, {0}},
    {0xb1, 3, 0, {0x01, 0x2c, 0x2d}},
    {0xb2, 3, 0, {0x01, 0x2c, 0x2d}},
    {0xb3, 6, 0, {0x01, 0x2c, 0x2d, 0x01, 0x2c, 0x2d}},
    {0xb4, 1, 0, {0x07}},
    {0xc0, 3, 0, {0xa2, 0x02, 0x84}},
    {0xc1, 1, 0, {0xc5}},
    {0xc2, 2, 0, {0x0a, 0x00}},
    {0xc3, 2, 0, {0x8a, 0x2a}},
    {0xc4, 2, 0, {0x8a, 0xee}},
    {0xc5, 1, 0, {0x0e}},
    {0xe0, 16, 0, {0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2b, 0x39, 0x00, 0x01, 0x03, 0x10}},
    {0xe1, 16, 0, {0x03, 0x1d, 0x07, 0x06, 0x2e, 0x2c, 0x29, 0x2d, 0x2e, 0x2e, 0x37, 0x3f, 0x00, 0x00, 0x02, 0x10}},
    {0x3a, 1, 0, {0x05}},
    {0x36, 1, 0, {0x68}},
    {0x21, 0, 0, {0}},
    {0x13, 0, 10, {0}},
};

void lcd_board_init(unsigned max_rows, lcd_board_sent_cb_t sent) {
    on_sent = sent;
    const gpio_config_t rst = {.pin_bit_mask = BIT64(PIN_LCD_RST), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    const gpio_config_t button = {.pin_bit_mask = BIT64(PIN_BUTTON), .mode = GPIO_MODE_INPUT,
                                  .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&button));

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
}

void lcd_board_backlight(bool on) { esp_lcd_panel_io_tx_param(io, on ? 0x29 : 0x28, NULL, 0); }

void lcd_board_draw(int y0, int y1, const uint16_t *pixels) {
    const int x0 = LCD_X_OFFSET, x1 = LCD_X_OFFSET + LCD_BOARD_W - 1;
    y0 += LCD_Y_OFFSET;
    y1 += LCD_Y_OFFSET;
    const uint8_t cols[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t rows[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)((y1 - 1) >> 8), (uint8_t)(y1 - 1)};
    esp_lcd_panel_io_tx_param(io, 0x2a, cols, sizeof(cols));
    esp_lcd_panel_io_tx_param(io, 0x2b, rows, sizeof(rows));
    esp_lcd_panel_io_tx_color(io, 0x2c, pixels, (size_t)LCD_BOARD_W * (y1 - y0) * sizeof(uint16_t));
}

/* The raw button: lcd_view_key() turns clicks and holds into keys. */
lcd_key_t lcd_board_read_key(void) { return gpio_get_level(PIN_BUTTON) ? LCD_KEY_NONE : LCD_KEY_ENTER; }

int lcd_board_battery(bool *charging) {
    *charging = false;
    return -1;
}

size_t lcd_board_input_status(char *out, size_t size) {
    return (size_t)snprintf(out, size, "LCDINPUT button %d key %d\n", gpio_get_level(PIN_BUTTON),
                            (int)lcd_board_read_key());
}

uint64_t lcd_board_pins(void) {
    return BIT64(PIN_LCD_SCLK) | BIT64(PIN_LCD_MOSI) | BIT64(PIN_LCD_CS) | BIT64(PIN_LCD_DC) |
           BIT64(PIN_LCD_RST) | BIT64(PIN_BUTTON);
}
