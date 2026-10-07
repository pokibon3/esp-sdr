/* ESP32-S3-BOX-Lite standalone spectrum view (CONFIG_ESP_SDR_LCD_VIEW). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { LCD_KEY_NONE, LCD_KEY_PREV, LCD_KEY_ENTER, LCD_KEY_NEXT } lcd_key_t;
/* ENTER cycles these; PREV/NEXT lower and raise the selected setting. */
typedef enum { LCD_MODE_CENTER, LCD_MODE_SPAN, LCD_MODE_STEP, LCD_MODE_GAIN, LCD_MODES } lcd_mode_t;

typedef struct {
    unsigned mhz;       /* LO frequency */
    unsigned rate_hz;   /* capture sample rate */
    unsigned span_mhz;  /* displayed span: the centered part of the capture */
    unsigned step_mhz;  /* CENTER step */
    unsigned gain;      /* manual gain index */
    lcd_mode_t mode;    /* highlighted setting */
    uint32_t status;    /* ring_status_t of the last run */
} lcd_view_info_t;

/* Call before burst_serial_init(): reserves the LCD and button GPIOs. */
void lcd_view_init(void);
/* Button press, or auto-repeat (*repeat set) while PREV/NEXT is held. */
lcd_key_t lcd_view_key(bool *repeat);
/* Route SPEC frames of the next ring_capture_run() into the view. */
void lcd_view_capture_begin(void);
/* Bins are LO-relative: a new LO or rate restarts the line and history. */
void lcd_view_capture_end(unsigned mhz, unsigned rate_hz);
/* Diagnostic snapshot of the last run (LCDDUMP? command). */
#include <stddef.h>
size_t lcd_view_dump(char *out, size_t size);
/* Blocks until the frame has been sent to the panel. */
void lcd_view_draw(const lcd_view_info_t *info);
