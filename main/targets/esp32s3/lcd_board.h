/* Board layer of the LCD view: one 320x240 panel and its input.
 * lcd_board_boxlite.c and lcd_board_cores3.c implement it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lcd_view.h"

#define LCD_BOARD_W 320
#define LCD_BOARD_H 240

/* Called from the transfer-done interrupt after each lcd_board_draw(). */
typedef bool (*lcd_board_sent_cb_t)(void);

/* Set up the panel (blank, backlight off) and the input; max_rows is the
 * tallest lcd_board_draw() band. */
void lcd_board_init(unsigned max_rows, lcd_board_sent_cb_t sent);
void lcd_board_backlight(bool on);
/* Queue full-width rows [y0, y1) of byte-swapped RGB565; returns at once. */
void lcd_board_draw(int y0, int y1, const uint16_t *pixels);
/* The input as a key, sampled now (no debouncing). */
lcd_key_t lcd_board_read_key(void);
/* Battery charge percent, or -1 without a battery or a fuel gauge. */
int lcd_board_battery(bool *charging);
/* Touch boards: the key for a touch at (x, y), in panel pixels. */
lcd_key_t lcd_view_key_at(int x, int y);
/* LCDINPUT? diagnostic: the raw input reading as text. */
#include <stddef.h>
size_t lcd_board_input_status(char *out, size_t size);
/* GPIOs the board uses, kept out of the host GPIO control set. */
uint64_t lcd_board_pins(void);
