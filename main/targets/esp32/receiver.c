/* Original ESP32 RX-only backend for the ESP-SDR burst protocol.
 * Register sequence derived from ESP-IDF's ESP32 librftest.a: adctrig/mac_init.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "burst_serial.h"
#include "burst_gpio.h"
#include "burst_version.h"
#include "spectrum.h"
#include "rx_recalibration.h"
#include "rx_tuning.h"
#include "rx_lo.h"
#include "rx_bandwidth.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "soc/rtc.h"
#include "soc/dport_reg.h"
#include "soc/dport_access.h"
#include "heap_memory_layout.h"
#if CONFIG_ESP_SDR_LCD_VIEW
#include <math.h>
#include "dsps_fft2r.h"
#include "spectrum_dc.h"
#include "spectrum_fft.h"
#include "spectrum_math.h"
#include "lcd_board.h"
#include "lcd_view.h"
#endif

/* Reserve both SRAM banks, including their instruction aliases. Mode 2 only
 * exposes 32 KiB to MAC; mode 3 exposes the entire 64 KiB capture aperture. */
SOC_RESERVE_MEMORY_REGION(0x3ffe8000, 0x3fff8000, esp32_rf_dump);
#define DUMP_CTRL 0x60033d90u
#define DUMP_STATUS 0x60033d94u
#define DUMP_BYTES 0x60033dc4u
#define RX_GAIN 0x3ff5c02cu
#define CAPACITY 16384u
#define MAX_SAMPLES (CAPACITY - 4)
#define SENTINEL 0xa55aa55au
static uint32_t *const samples = (void *)0x3ffe8000;
static unsigned gain_max, gain_code = 40;
static bool hardware_agc = true;
static unsigned frequency_mhz=2412;
/* Original ESP32 BBTOP RX filter: registers 1/2, seven-bit capacitor DAC.
 * Preserve bit 7 and restore PHY calibration before any retune or transfer. */
static int rx_filter = -1;
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);
static unsigned filter_saved[2];
static void filter_apply(void) {
    for (unsigned j=0; j<2; j++) {
        filter_saved[j]=rom_chip_i2c_readReg(0x67, 1, 1+j);
        if (rx_filter>=0) rom_chip_i2c_writeReg(0x67, 1, 1+j, (filter_saved[j]&~127u)|(unsigned)rx_filter);
    }
    esp_rom_delay_us(10);
}
static void filter_restore(void) {
    if (rx_filter>=0) for (unsigned j=0; j<2; j++)
        rom_chip_i2c_writeReg(0x67, 1, 1+j, filter_saved[j]);
}
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(int);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(int);

/* The LCD view's captures have no host to report errors to. */
static bool lcd_quiet;
static void reply(const char *fmt, ...) {
    if (lcd_quiet) return;
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (length > 0) burst_serial_send(text, length < sizeof(text) ? length : sizeof(text)-1);
}

static void prepare_rx(void) {
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    if(rx_lo_select(rx_lo_plan(frequency_mhz).alternate))esp_rom_delay_us(3000);
}

/* Clock selectors: 0=16 MS/s, 1=80 MS/s, 2=40 MS/s. Hardware sampling,
 * not software decimation. Rates inferred from capture-duration slopes. */
static bool acquire_iq(unsigned n, unsigned source, unsigned clock, unsigned *capture_us) {
    if (n < 256 || n > MAX_SAMPLES || source > 3 || clock > 2) {
        reply("ERR args\n");
        return false;
    }
    REG_WRITE(DUMP_CTRL, 0);
    for (unsigned j = 0; j < CAPACITY; j++) samples[j] = SENTINEL;
    filter_apply();
    uint32_t owner = DPORT_REG_READ(DPORT_IRAM_DRAM_AHB_SEL_REG);
    uint32_t byte_select = REG_READ(DUMP_BYTES);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG,
        (owner & ~DPORT_MAC_DUMP_MODE_M) | (3 << DPORT_MAC_DUMP_MODE_S));
    REG_WRITE(DUMP_BYTES, 0x03020100); /* Four distinct bytes, as in mac_init. */
    uint32_t ctrl = n | (source << 20) | ((clock == 0) << 16) | ((clock == 2) << 15);
    REG_WRITE(DUMP_CTRL, ctrl | BIT(31));
    int64_t start = esp_timer_get_time();
    REG_SET_BIT(DUMP_CTRL, BIT(19));
    REG_CLR_BIT(DUMP_CTRL, BIT(19));
    while (!(REG_READ(DUMP_CTRL) & BIT(18)) && esp_timer_get_time()-start < 20000) {}
    uint32_t result = REG_READ(DUMP_CTRL), status = REG_READ(DUMP_STATUS);
    unsigned elapsed = esp_timer_get_time()-start;
    REG_WRITE(DUMP_CTRL, 0);
    esp_rom_delay_us(10);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, owner);
    REG_WRITE(DUMP_BYTES, byte_select);
    filter_restore();
    if (!(result & BIT(18)) || (status & 0x7fff) != n) {
        reply("ERR capture_timeout\n");
        return false;
    }
    for (unsigned j = 0; j < CAPACITY; j++) {
        if ((j < n && samples[j] == SENTINEL) || (j >= n && samples[j] != SENTINEL)) {
            reply("ERR capture_memory %u\n", j);
            return false;
        }
    }
    *capture_us = elapsed;
    return true;
}

static bool spectrum_acquire(unsigned n, unsigned rate, const uint32_t **data, unsigned *elapsed) {
    unsigned clock = rate == 6 ? 0 : rate == 1 ? 2 : 1;
    bool ok = acquire_iq(n, 0, clock, elapsed);
    *data = samples;
    return ok;
}

#include "ring_probe.h"

static bool capture(unsigned n, unsigned source, unsigned clock, unsigned bits) {
    unsigned elapsed;
    if (!acquire_iq(n, source, clock, &elapsed)) return false;
    size_t bytes = n * 4;
    uint8_t *packed = (uint8_t *)samples;
    if (bits == 8) {
        for (unsigned j = 0; j < n; j++) {
            uint32_t w = samples[j];
            packed[2*j] = (w >> 2) & 255;
            packed[2*j+1] = (w >> 12) & 255;
        }
        bytes = n * 2;
    } else if (bits == 10) {
        for (unsigned j = 0; j < n; j += 2, packed += 5) {
            uint32_t a = samples[j] & 0xfffff;
            uint32_t b = j+1 < n ? samples[j+1] & 0xfffff : 0;
            packed[0] = a; packed[1] = a >> 8; packed[2] = (a >> 16) | (b << 4);
            if (j+1 < n) { packed[3] = b >> 4; packed[4] = b >> 12; }
        }
        bytes = (n * 20 + 7) / 8;
    }
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)samples, bytes);
    char header[96];
    int length = snprintf(header, sizeof(header), "DATA %u %08"PRIx32" %u\n", n, crc, elapsed);
    return burst_serial_send(header, length) && burst_serial_send(samples, bytes);
}

static void apply_gain(void) {
    uint32_t gain = REG_READ(RX_GAIN);
    if (hardware_agc) REG_CLR_BIT(RX_GAIN, BIT(23));
    else REG_WRITE(RX_GAIN, (gain & 0x7fffff) | (gain_code << 24) | BIT(23));
}

extern void set_chanfreq(unsigned mhz, unsigned mode);
extern void rom_set_rf_freq_offset(unsigned crystal, unsigned mhz, int offset);

static void tune_rx(unsigned mhz) {
    static unsigned calibrated_mhz;
    if (calibrated_mhz != mhz) {
        rx_recalibrate(mhz);
        calibrated_mhz = mhz;
    }
    rx_lo_plan_t plan=rx_lo_plan(mhz);
    rx_lo_select(false);
    bool channel = (mhz >= 2412 && mhz <= 2472 && (mhz-2412)%5 == 0) || mhz == 2484;
    /* Calibrate on a real channel before bypassing the channel-number mapping.
     * Use the PHY entry point even for repeated requests: the Wi-Fi API can
     * skip a channel it thinks is already selected after a direct PLL retune. */
    set_chanfreq(channel ? mhz : 2412, 0);
    if (!channel) {
        unsigned xtal = rtc_clk_xtal_freq_get();
        /* Channel setup pins its capacitor code. Release it before the ROM
         * retune, otherwise the SDM changes but the PLL can remain unlocked. */
        unsigned cap=ram_chip_i2c_readReg(0x62,1,0);
        ram_chip_i2c_writeReg(0x62,1,0,cap&~0x80u);
        /* This ROM uses 1/1024 MHz offsets, unlike the newer kHz API. */
        int offset=(plan.offset_khz*1024+500)/1000;
        rom_set_rf_freq_offset(xtal == 26 ? 1 : xtal == 24 ? 2 : 0, plan.mhz, offset);
    }
}

static bool capture_rate(unsigned n, unsigned rate, unsigned format) {
    if (n < 256 || n > MAX_SAMPLES || (rate != 0 && rate != 1 && rate != 6)) {
        reply("ERR capture_settings\n"); return false;
    }
    return capture(n, 0, rate == 0 ? 1 : rate == 1 ? 2 : 0, format);
}

#if CONFIG_ESP_SDR_LCD_VIEW
/* Standalone LCD view: snapshots for about LCD_RUN_MS, each cut into 256-bin
 * FFTs, then a redraw. Settings, gain hold and filter follow the S3 view;
 * the gain index is not characterized on the ESP32. */
#define LCD_RUN_MS 40u
#define LCD_BINS 256u
#define LCD_GAIN_DEFAULT 35u
#define LCD_GAIN_STEP 10u
#define LCD_FILTER_MHZ 40u
static bool lcd_gain_held, lcd_host_agc;
static unsigned lcd_host_gain;
static void lcd_release_gain(void) {
    if (!lcd_gain_held) return;
    hardware_agc = lcd_host_agc; gain_code = lcd_host_gain; apply_gain(); lcd_gain_held = false;
}
static const unsigned lcd_spans[] = {10, 20, 40, 80}, lcd_steps[] = {1, 5, 10, 20};
static unsigned lcd_mode = LCD_MODE_CENTER, lcd_span = 3, lcd_step = 2, lcd_gain = LCD_GAIN_DEFAULT;
/* 16, 40 and 80 MS/s: the 10 and 20 MHz spans show the centre of a 16 or
 * 40 MS/s capture. */
static unsigned lcd_rate(void) { return lcd_spans[lcd_span] >= 80 ? 0 : lcd_spans[lcd_span] >= 20 ? 1 : 6; }
static unsigned lcd_rate_hz(unsigned rate) { return rate == 0 ? 80000000 : rate == 1 ? 40000000 : 16000000; }
static unsigned lcd_adjust(unsigned i, unsigned count, int dir) {
    return dir < 0 ? (i ? i - 1 : 0) : (i + 1 < count ? i + 1 : i);
}
static void lcd_tune(unsigned mhz) {
    frequency_mhz = mhz; tune_rx(mhz); prepare_rx(); apply_gain();
}
static void lcd_no_poll(void *context) { (void)context; }
/* Same scaling as SPEC (spectrum.c), so the codes match the host's dBFS. */
static bool lcd_capture(unsigned rate) {
    static int16_t fft[2 * LCD_BINS] __attribute__((aligned(16)));
    static int16_t window[LCD_BINS];
    static uint8_t codes[LCD_BINS];
    if (!window[LCD_BINS / 2]) {
        if (!spectrum_fft_init()) return false;
        for (unsigned j = 0; j < LCD_BINS; j++) window[j] = (int16_t)lrintf(16383.5f * (1 - cosf(2 * M_PI * j / LCD_BINS)));
    }
    const unsigned n = MAX_SAMPLES / LCD_BINS * LCD_BINS;
    spectrum_dc_t dc = {0};
    const int64_t start = esp_timer_get_time();
    do {
        const uint32_t *words; unsigned elapsed;
        if (!spectrum_acquire(n, rate, &words, &elapsed)) return false;
        for (unsigned at = 0; at < n; at += LCD_BINS) {
            for (unsigned j = 0; j < LCD_BINS; j++) {
                const uint32_t w = words[at + j];
                const int32_t i = (int32_t)(w << 22) >> 22, q = (int32_t)(w << 12) >> 22;
                fft[2 * j] = spectrum_sat16((i * window[j]) >> 9);
                fft[2 * j + 1] = spectrum_sat16((q * window[j]) >> 9);
            }
            spectrum_fft_poll(fft, LCD_BINS, dsps_fft_w_table_sc16, lcd_no_poll, NULL);
            spectrum_dc_apply(&dc, fft, LCD_BINS);
            /* The FFT leaves bit-reversed order. */
            for (unsigned j = 0, bin = 0; j < LCD_BINS; j++) {
                codes[bin] = spectrum_power_code(spectrum_complex_power(fft[2 * j], fft[2 * j + 1]));
                unsigned bit = LCD_BINS >> 1;
                while (bin & bit) { bin ^= bit; bit >>= 1; }
                bin ^= bit;
            }
            lcd_view_add_spectrum(codes);
        }
    } while (esp_timer_get_time() - start < LCD_RUN_MS * 1000);
    return true;
}
static void lcd_cycle(void) {
    bool repeat;
    lcd_key_t key = lcd_view_key(&repeat);
    if (key >= LCD_KEY_SELECT) lcd_mode = (unsigned)(key - LCD_KEY_SELECT) % LCD_MODES;
    else if (key == LCD_KEY_ENTER) lcd_mode = (lcd_mode + 1) % LCD_MODES;
    else if (key == LCD_KEY_PREV || key == LCD_KEY_NEXT) {
        const int dir = key == LCD_KEY_NEXT ? 1 : -1;
        if (lcd_mode == LCD_MODE_CENTER) {
            int f = (int)frequency_mhz + dir * (int)lcd_steps[lcd_step];
            if (f < (int)RX_FREQ_MIN) f = RX_FREQ_MIN;
            if (f > (int)RX_FREQ_MAX) f = RX_FREQ_MAX;
            if ((unsigned)f != frequency_mhz) lcd_tune((unsigned)f);
        } else if (!repeat && lcd_mode == LCD_MODE_SPAN) lcd_span = lcd_adjust(lcd_span, 4, dir);
        else if (!repeat && lcd_mode == LCD_MODE_STEP) lcd_step = lcd_adjust(lcd_step, 4, dir);
        else if (lcd_mode == LCD_MODE_GAIN) {
            int g = (int)lcd_gain + dir * (int)LCD_GAIN_STEP;
            if (g >= 0 && g <= (int)gain_max) lcd_gain = (unsigned)g;
        }
    }
    if (!lcd_gain_held) { lcd_host_agc = hardware_agc; lcd_host_gain = gain_code; lcd_gain_held = true; }
    if (hardware_agc || gain_code != lcd_gain) { hardware_agc = false; gain_code = lcd_gain; apply_gain(); }
    const unsigned rate = lcd_rate();
    /* Under the host's automatic filter, wide spans would lose their edges. */
    const int host_filter = rx_filter;
    if (host_filter < 0 && lcd_spans[lcd_span] >= 40) rx_filter = rx_bandwidth_dcap(LCD_FILTER_MHZ);
    lcd_quiet = true;
    lcd_view_capture_begin();
    const bool ok = lcd_capture(rate);
    lcd_view_capture_end(frequency_mhz, lcd_rate_hz(rate));
    lcd_quiet = false;
    rx_filter = host_filter;
    const lcd_view_info_t info = {.mhz = frequency_mhz, .rate_hz = lcd_rate_hz(rate), .span_mhz = lcd_spans[lcd_span],
                                  .step_mhz = lcd_steps[lcd_step], .gain = gain_code, .mode = (lcd_mode_t)lcd_mode,
                                  .status = !ok, .battery = -1};
    lcd_view_draw(&info);
}
#endif

static void command(const char *line) {
#if CONFIG_ESP_SDR_LCD_VIEW
    if (!strcmp(line, "LCDDUMP?")) { char h[1600]; lcd_view_dump(h, sizeof(h)); burst_serial_send(h, strlen(h)); return; }
    if (!strcmp(line, "LCDINPUT?")) { char h[160]; lcd_board_input_status(h, sizeof(h)); reply("%s", h); return; }
#endif
    if (burst_version_command(line)) return;
    if (burst_gpio_command(line)) return;
#ifdef RING_PROBE
    if(ring_probe_command(line)) return;
#endif
    if (spectrum_command(line, frequency_mhz, spectrum_acquire)) return;
    unsigned n, rate, repeats, format;
    uint64_t nonce;
    char extra;
    if (!strcmp(line, "INFO")) reply("ESP32SDR 6 burst 16380\n");
    else if (!strcmp(line, "CAPS")) reply("CAPS VERSION GPIO SPEC SPECN SPECCAPS SPECSTAT DCT UARTBAUD RXLIMITS SERIALLEASE TUNEEXT RX40 RX16 LPFANA GAIN HWAGC IQ8\n");
    else if (sscanf(line, "BANDWIDTH %u %c", &n, &extra)==1 &&
             (!n || (n>=RX_BANDWIDTH_MIN && n<=RX_BANDWIDTH_MAX))) {
        rx_filter=rx_bandwidth_dcap(n); reply("OK\n");
    }
    else if (!strcmp(line, "LPF AUTO")) { rx_filter=-1; reply("OK\n"); }
    else if (sscanf(line, "LPF %u %c", &n, &extra)==1 && n<=127) { rx_filter=n; reply("OK\n"); }
    else if (!strcmp(line, "LPF?")) reply("LPF %d %u %u\n", rx_filter,
        rom_chip_i2c_readReg(0x67,1,1), rom_chip_i2c_readReg(0x67,1,2));
    else if (!strcmp(line, "TRANSPORT?")) reply("TRANSPORT UART %u\n", burst_serial_baud());
    else if (!strcmp(line, "LIMITS?")) {
        reply("LIMITS {\"gain\":[0,%u,1],\"bandwidth\":[%u,%u,1,0],\"rates\":[80000000,40000000,16000000],\"bits\":[8,10]}\n", gain_max, RX_BANDWIDTH_MIN, RX_BANDWIDTH_MAX);
    } else if (!strcmp(line, "RANGE?")) reply(RX_TUNING_RANGE_REPLY);
    else if (sscanf(line, "SYNC %"SCNu64" %c", &nonce, &extra) == 1) reply("SYNC %"PRIu64"\n", nonce);
    else if (!strcmp(line, "RELEASE")) reply("OK\n");
    else if (!strcmp(line, "GAIN?")) reply("GAIN %s %d 0 %u %u\n",
        hardware_agc ? "HARDWARE" : "MANUAL", hardware_agc ? -1 : (int)gain_code,
        gain_max, (unsigned)((REG_READ(RX_GAIN) >> 23) & 1));
    else if (!strcmp(line, "GAIN HARDWARE")) { hardware_agc = true; apply_gain(); reply("OK\n"); }
    else if (sscanf(line, "GAIN MANUAL %u %c", &n, &extra) == 1 && n <= gain_max) {
        hardware_agc = false; gain_code = n; apply_gain(); reply("OK\n");
    } else if (sscanf(line, "FREQ %u %c", &n, &extra) == 1 && rx_frequency_valid(n)) {
        frequency_mhz=n; tune_rx(n);
        prepare_rx(); apply_gain(); reply("OK\n");
    } else if (sscanf(line, "CAP16 %u %u %c", &n, &rate, &extra) == 2) capture_rate(n, rate, 8);
    else if (sscanf(line, "CAP20 %u %u %c", &n, &rate, &extra) == 2) capture_rate(n, rate, 10);
    else if (!strncmp(line, "CAP ", 4) && sscanf(line, "CAP %u %u %c", &n, &rate, &extra) == 2) capture_rate(n, rate, 32);
    else if (sscanf(line, "RXRUN %u %u %u %u %c", &n, &rate, &repeats, &format, &extra) == 4 &&
             repeats > 0 && repeats <= 1000 && (format == 16 || format == 20)) {
        for (unsigned j = 0; j < repeats; j++) {
            if (!capture_rate(n, rate, format/2)) return;
            vTaskDelay(1);
        }
        reply("END\n");
    } else reply("ERR command\n");
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    prepare_rx();
    gain_max = (REG_READ(RX_GAIN) >> 8) & 127;
    REG_CLR_BIT(RX_GAIN, BIT(23)); /* Hardware AGC is the default. */
    fflush(stdout);
#if CONFIG_ESP_SDR_LCD_VIEW
    lcd_view_init(); /* before GPIO control claims the free pins */
    /* Start centered on the 2.4 GHz Wi-Fi band (ch7). */
    lcd_tune(2442);
#endif
    burst_serial_init();
    char line[128];
#if CONFIG_ESP_SDR_LCD_VIEW
    /* A host command holds off the LCD view until RELEASE or five seconds
     * of silence. */
    int64_t lease_deadline = 0;
#endif
    for (;;) {
        int status = burst_serial_poll_line(line, sizeof(line));
        if (!status) {
#if CONFIG_ESP_SDR_LCD_VIEW
            if (esp_timer_get_time() >= lease_deadline) { lcd_cycle(); continue; }
#endif
            vTaskDelay(1); continue;
        }
#if CONFIG_ESP_SDR_LCD_VIEW
        lcd_release_gain(); /* the host sees its own gain mode */
#endif
        if (status < 0) reply("ERR command_length\n");
        else command(line);
#if CONFIG_ESP_SDR_LCD_VIEW
        lease_deadline = status > 0 && !strcmp(line, "RELEASE") ? 0 : esp_timer_get_time() + 5000000;
#endif
    }
}
