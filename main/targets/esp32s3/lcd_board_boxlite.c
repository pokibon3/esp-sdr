/* ESP32-S3-BOX-Lite board layer: ST7789 (320x240 landscape, SPI3) and the
 * three front buttons on an ADC1 ladder (GPIO1). Pins, orientation and
 * button voltages follow espressif/esp-bsp bsp/esp-box-lite. */
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"

#include "lcd_board.h"

#define PIN_LCD_MOSI 6
#define PIN_LCD_SCLK 7
#define PIN_LCD_CS 5
#define PIN_LCD_DC 4
#define PIN_LCD_RST 48
#define PIN_LCD_BL 45          /* active low */
#define PIN_BUTTONS 1          /* ADC1 channel 0 */
#define LCD_HOST SPI3_HOST
#define LCD_PCLK_HZ 40000000

static esp_lcd_panel_handle_t panel;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t adc_cali;
static lcd_board_sent_cb_t on_sent;

static bool color_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    (void)io; (void)e; (void)ctx;
    return on_sent();
}

void lcd_board_init(unsigned max_rows, lcd_board_sent_cb_t sent) {
    on_sent = sent;
    gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_BL, 1);
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
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    const adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &chan));
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_0, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &adc_cali) != ESP_OK) adc_cali = NULL;
}

void lcd_board_backlight(bool on) { gpio_set_level(PIN_LCD_BL, on ? 0 : 1); }

void lcd_board_draw(int y0, int y1, const uint16_t *pixels) {
    esp_lcd_panel_draw_bitmap(panel, 0, y0, LCD_BOARD_W, y1, pixels);
}

/* Left PREV, middle ENTER, right NEXT. */
lcd_key_t lcd_board_read_key(void) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc, ADC_CHANNEL_0, &raw) != ESP_OK) return LCD_KEY_NONE;
    if (!adc_cali || adc_cali_raw_to_voltage(adc_cali, raw, &mv) != ESP_OK) mv = raw * 3100 / 4095;
    if (mv >= 2310 && mv <= 2510) return LCD_KEY_PREV;
    if (mv >= 1880 && mv <= 2080) return LCD_KEY_ENTER;
    if (mv >= 720 && mv <= 920) return LCD_KEY_NEXT;
    return LCD_KEY_NONE;
}

size_t lcd_board_input_status(char *out, size_t size) {
    int raw = 0, mv = 0;
    esp_err_t e = adc_oneshot_read(adc, ADC_CHANNEL_0, &raw);
    if (!adc_cali || adc_cali_raw_to_voltage(adc_cali, raw, &mv) != ESP_OK) mv = raw * 3100 / 4095;
    return (size_t)snprintf(out, size, "LCDINPUT adc err %d raw %d mv %d key %d\n", e, raw, mv, (int)lcd_board_read_key());
}

uint64_t lcd_board_pins(void) {
    return BIT64(PIN_LCD_MOSI) | BIT64(PIN_LCD_SCLK) | BIT64(PIN_LCD_CS) | BIT64(PIN_LCD_DC) |
           BIT64(PIN_LCD_RST) | BIT64(PIN_LCD_BL) | BIT64(PIN_BUTTONS);
}
