/* M5Stack CoreS3 board layer: ILI9342C/E (320x240, SPI3) powered through
 * the AXP2101 PMIC and AW9523 I/O expander, with an FT6336 touch panel on
 * the internal I2C bus (its touch area extends below the panel, to y 279).
 * Pins, power and panel setup follow espressif/esp-bsp
 * bsp/m5stack_core_s3 (Apache-2.0); the ILI9342E commands are copied from
 * its priv_include/ili9342e_init_cmds.h. The board has no buttons: touches
 * map to keys through lcd_view_key_at(). */
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_board.h"

#define PIN_I2C_SDA 12
#define PIN_I2C_SCL 11
#define PIN_LCD_SCLK 36
#define PIN_LCD_MOSI 37
#define PIN_LCD_DC 35
#define PIN_LCD_CS 3
#define LCD_HOST SPI3_HOST
#define LCD_PCLK_HZ 40000000
#define I2C_TIMEOUT_MS 50

#define AXP2101_ADDR 0x34
#define AXP2101_STATUS1 0x00    /* bit 3: battery present */
#define AXP2101_STATUS2 0x01    /* bits 6:5: 01 charging, 10 discharging */
#define AXP2101_GAUGE_EN 0x18   /* bit 3: fuel gauge */
#define AXP2101_BATTERY 0xa4    /* fuel gauge charge, percent */
#define AXP2101_LDO_EN 0x90     /* bit 7: DLDO1 (backlight) */
#define AXP2101_DLDO1_V 0x99    /* (mV - 500) / 100 */
#define AW9523_ADDR 0x58
#define AW9523_OUT_P0 0x02
#define AW9523_OUT_P1 0x03
#define AW9523_DIR_P0 0x04      /* 1: input */
#define AW9523_DIR_P1 0x05
#define AW9523_GCR 0x11         /* bit 4: P0 push-pull */
#define AW9523_TOUCH_EN 0x01    /* P0_0 */
#define AW9523_LCD_EN 0x02      /* P1_1, also the panel reset */
#define FT6336_ADDR 0x38
#define FT6336_TD_STATUS 0x02
#define FT6336_FIRMID 0xa6
#define FT6336_FIRMID_ILI9342E 0x12

static i2c_master_bus_handle_t i2c;
static i2c_master_dev_handle_t axp, expander, touch;
static esp_lcd_panel_io_handle_t io;
static lcd_board_sent_cb_t on_sent;

static void reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    const uint8_t data[2] = {reg, value};
    ESP_ERROR_CHECK(i2c_master_transmit(dev, data, sizeof(data), I2C_TIMEOUT_MS));
}
static uint8_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg) {
    uint8_t value = 0;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, &reg, 1, &value, 1, I2C_TIMEOUT_MS));
    return value;
}
static void reg_update(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t value) {
    reg_write(dev, reg, (uint8_t)((reg_read(dev, reg) & ~mask) | (value & mask)));
}
static i2c_master_dev_handle_t add_device(uint8_t address, uint32_t hz) {
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address, .scl_speed_hz = hz,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c, &config, &dev));
    return dev;
}

static bool color_sent(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    (void)panel_io; (void)e; (void)ctx;
    return on_sent();
}

typedef struct {
    uint8_t cmd, len, delay_ms;
    uint8_t data[15];
} panel_cmd_t;

/* esp_lcd_ili9341's default sequence, used by both panel versions. */
static const panel_cmd_t ili9341_cmds[] = {
    {0xcf, 3, 0, {0x00, 0xaa, 0xe0}},
    {0xed, 4, 0, {0x67, 0x03, 0x12, 0x81}},
    {0xe8, 3, 0, {0x8a, 0x01, 0x78}},
    {0xcb, 5, 0, {0x39, 0x2c, 0x00, 0x34, 0x02}},
    {0xf7, 1, 0, {0x20}},
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
};
/* Added on ILI9342E boards (touch FIRMID 0x12). */
static const panel_cmd_t ili9342e_cmds[] = {
    {0xdd, 1, 0, {0x01}},
    {0xd5, 1, 0, {0x00}},
    {0xb1, 1, 0, {0x22}},
    {0xc8, 1, 0, {0x38}},
    {0xcb, 1, 0, {0x1c}},
    {0xc9, 1, 0, {0x1a}},
    {0xca, 1, 0, {0x1a}},
    {0xb7, 4, 0, {0x5a, 0x41, 0x11, 0x19}},
    {0xe4, 15, 0, {0x04, 0x08, 0x11, 0x06, 0x12, 0x07, 0x3a, 0x76, 0x47, 0x07, 0x0f, 0x0a, 0x11, 0x19, 0x05}},
    {0xe5, 15, 0, {0x02, 0x03, 0x07, 0x06, 0x12, 0x07, 0x36, 0x5f, 0x48, 0x06, 0x10, 0x0c, 0x16, 0x14, 0x09}},
};
/* 16-bit pixels, BGR, native landscape, inverted (IPS), then wake. */
static const panel_cmd_t start_cmds[] = {
    {0x3a, 1, 0, {0x55}},
    {0x36, 1, 0, {0x08}},
    {0x21, 0, 0, {0}},
    {0x11, 0, 120, {0}},
    {0x29, 0, 20, {0}},
};
static void send_cmds(const panel_cmd_t *cmds, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmds[i].cmd, cmds[i].len ? cmds[i].data : NULL, cmds[i].len));
        if (cmds[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(cmds[i].delay_ms));
    }
}

/* The touch firmware identifies the panel; 0 when it does not answer. */
static uint8_t touch_firmid(void) {
    for (int retry = 0; retry < 5; retry++) {
        const uint8_t work_mode[2] = {0x00, 0x00};
        uint8_t reg = FT6336_FIRMID, id = 0;
        if (i2c_master_transmit(touch, work_mode, sizeof(work_mode), I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_master_transmit_receive(touch, &reg, 1, &id, 1, I2C_TIMEOUT_MS) == ESP_OK && id)
            return id;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 0;
}

void lcd_board_init(unsigned max_rows, lcd_board_sent_cb_t sent) {
    on_sent = sent;
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &i2c));
    axp = add_device(AXP2101_ADDR, 400000);
    expander = add_device(AW9523_ADDR, 400000);
    touch = add_device(FT6336_ADDR, 100000);

    lcd_board_backlight(false);
    reg_update(axp, AXP2101_GAUGE_EN, 0x08, 0x08);
    reg_write(axp, AXP2101_DLDO1_V, (3300 - 500) / 100);
    /* Touch on; LCD enable pulsed low as the panel's hardware reset. */
    reg_update(expander, AW9523_GCR, 0x10, 0x10);
    reg_update(expander, AW9523_OUT_P0, AW9523_TOUCH_EN, AW9523_TOUCH_EN);
    reg_update(expander, AW9523_DIR_P0, AW9523_TOUCH_EN, 0);
    reg_update(expander, AW9523_OUT_P1, AW9523_LCD_EN, 0);
    reg_update(expander, AW9523_DIR_P1, AW9523_LCD_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    reg_update(expander, AW9523_OUT_P1, AW9523_LCD_EN, AW9523_LCD_EN);
    vTaskDelay(pdMS_TO_TICKS(300)); /* panel reset and touch start-up */

    const spi_bus_config_t spi = {
        .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_BOARD_W * max_rows * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS, .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4,
        .on_color_trans_done = color_sent,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));
    send_cmds(ili9341_cmds, sizeof(ili9341_cmds) / sizeof(ili9341_cmds[0]));
    if (touch_firmid() == FT6336_FIRMID_ILI9342E) send_cmds(ili9342e_cmds, sizeof(ili9342e_cmds) / sizeof(ili9342e_cmds[0]));
    send_cmds(start_cmds, sizeof(start_cmds) / sizeof(start_cmds[0]));
}

void lcd_board_backlight(bool on) {
    reg_update(axp, AXP2101_LDO_EN, 0x80, on ? 0x80 : 0);
}

void lcd_board_draw(int y0, int y1, const uint16_t *pixels) {
    const uint8_t cols[4] = {0, 0, (LCD_BOARD_W - 1) >> 8, (LCD_BOARD_W - 1) & 0xff};
    const uint8_t rows[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)((y1 - 1) >> 8), (uint8_t)(y1 - 1)};
    esp_lcd_panel_io_tx_param(io, 0x2a, cols, sizeof(cols));
    esp_lcd_panel_io_tx_param(io, 0x2b, rows, sizeof(rows));
    esp_lcd_panel_io_tx_color(io, 0x2c, pixels, (size_t)LCD_BOARD_W * (y1 - y0) * sizeof(uint16_t));
}

int lcd_board_battery(bool *charging) {
    const uint8_t regs[3] = {AXP2101_STATUS1, AXP2101_STATUS2, AXP2101_BATTERY};
    uint8_t v[3];
    *charging = false;
    for (int i = 0; i < 3; i++)
        if (i2c_master_transmit_receive(axp, &regs[i], 1, &v[i], 1, I2C_TIMEOUT_MS) != ESP_OK) return -1;
    if (!(v[0] & 0x08)) return -1;
    *charging = (v[1] >> 5 & 3) == 1;
    return v[2] > 100 ? 100 : v[2];
}

lcd_key_t lcd_board_read_key(void) {
    uint8_t reg = FT6336_TD_STATUS, p[5];
    if (i2c_master_transmit_receive(touch, &reg, 1, p, sizeof(p), I2C_TIMEOUT_MS) != ESP_OK) return LCD_KEY_NONE;
    unsigned points = p[0] & 0x0f;
    if (!points || points > 2) return LCD_KEY_NONE;
    return lcd_view_key_at((p[1] & 0x0f) << 8 | p[2], (p[3] & 0x0f) << 8 | p[4]);
}

size_t lcd_board_input_status(char *out, size_t size) {
    uint8_t reg = 0x00, p[7] = {0};
    esp_err_t e = i2c_master_transmit_receive(touch, &reg, 1, p, sizeof(p), I2C_TIMEOUT_MS);
    uint8_t id_reg = FT6336_FIRMID, id = 0;
    esp_err_t e2 = i2c_master_transmit_receive(touch, &id_reg, 1, &id, 1, I2C_TIMEOUT_MS);
    bool charging;
    int battery = lcd_board_battery(&charging);
    return (size_t)snprintf(out, size, "LCDINPUT touch err %d %02x%02x%02x%02x%02x%02x%02x id err %d %02x p0 %02x p1 %02x dir %02x %02x key %d axp %02x %02x %02x battery %d%s\n",
                            e, p[0], p[1], p[2], p[3], p[4], p[5], p[6], e2, id, reg_read(expander, AW9523_OUT_P0),
                            reg_read(expander, AW9523_OUT_P1), reg_read(expander, AW9523_DIR_P0),
                            reg_read(expander, AW9523_DIR_P1), (int)lcd_board_read_key(), reg_read(axp, AXP2101_STATUS1),
                            reg_read(axp, AXP2101_STATUS2), reg_read(axp, AXP2101_GAUGE_EN), battery, charging ? " charging" : "");
}

uint64_t lcd_board_pins(void) {
    return BIT64(PIN_I2C_SDA) | BIT64(PIN_I2C_SCL) | BIT64(PIN_LCD_SCLK) | BIT64(PIN_LCD_MOSI) |
           BIT64(PIN_LCD_DC) | BIT64(PIN_LCD_CS);
}
