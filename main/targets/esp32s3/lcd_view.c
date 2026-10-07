/* Standalone LCD spectrum view (CONFIG_ESP_SDR_LCD_VIEW).
 *
 * The receiver runs short 256-bin SPEC captures with the output queue routed
 * here instead of USB, then draws between runs with interrupts enabled. The
 * panel and input come from the board layer (lcd_board.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lcd_board.h"
#include "lcd_font.h"
#include "lcd_view.h"

#define W LCD_BOARD_W
#define H LCD_BOARD_H
#define HDR_H 26                /* mode boxes: label row, then a large value */
#define VAL_Y 10                /* top row of the values (LCD_FONT_H tall) */
#define BOX_W (W / LCD_MODES)
#define SP_Y HDR_H              /* spectrum trace */
#define SP_H 130
#define AX_Y (SP_Y + SP_H)      /* Wi-Fi channels, then frequency labels */
#define AX_H 24
#define WF_Y (AX_Y + AX_H)      /* waterfall, newest row on top */
#define WF_H (H - WF_Y)
#define STRIPE 8                /* rows per SPI color transfer */
_Static_assert(H % STRIPE == 0, "stripes");
#define TEXT_ROWS (HDR_H + AX_H)
#define SCALE_W 48              /* dBFS labels over the left of the trace */

#define BINS 256u
#define SPEC_MAGIC 0x31435053u  /* "SPC1" */
#define SPEC_HDR 28u
/* Host viewer (esp-web-sdr specToDbfs): dBFS = code / 2 - 84.3. */
#define CODE_DBFS_X10(c) ((int)(c) * 5 - 843)
#define SP_TOP_DB (-20)         /* fixed trace scale, dBFS */
#define SP_BOTTOM_DB (-80)
#define SP_GRID_DB 20
#define WF_LOW_DB 10            /* waterfall colors: noise-tracked bottom + 10 .. + 50 dB */
#define WF_RANGE_DB 40

/* ---- SPEC frame sink (core 0, interrupts masked) ----------------------- */

int (*ring_local_sink)(const uint8_t *p, unsigned n);

static uint8_t frame_hdr[SPEC_HDR];
static unsigned frame_pos, frame_len;
static bool frame_skip;
static uint8_t *acc;                   /* per-bin peak; heap: see the display state note */
static uint32_t acc_frames;

/* Frames arrive as a byte stream in arbitrary chunks, starting at a frame
 * boundary each run. Keep each bin's highest 0.5 dB code: a run spans tens
 * of sub-millisecond frames, and averaging them buries Wi-Fi bursts. */
IRAM_ATTR static int view_sink(const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n;) {
        if (frame_pos < SPEC_HDR) {
            frame_hdr[frame_pos++] = p[i++];
            if (frame_pos == SPEC_HDR) {
                uint32_t magic = frame_hdr[0] | frame_hdr[1] << 8 | frame_hdr[2] << 16 | (uint32_t)frame_hdr[3] << 24;
                frame_len = SPEC_HDR + (1u << (frame_hdr[26] & 15u)) + 4u;
                frame_skip = magic != SPEC_MAGIC || frame_hdr[26] != 8;
            }
            continue;
        }
        unsigned take = n - i, left = frame_len - frame_pos;
        if (take > left) take = left;
        if (!frame_skip) {
            for (unsigned k = 0; k < take; k++) {
                unsigned b = frame_pos - SPEC_HDR + k;
                if (b < BINS && p[i + k] > acc[b]) acc[b] = p[i + k];
            }
        }
        frame_pos += take;
        i += take;
        if (frame_pos == frame_len) {
            if (!frame_skip) acc_frames++;
            frame_pos = 0;
        }
    }
    return (int)n;
}

/* ---- display state ------------------------------------------------------ */

static SemaphoreHandle_t free_stripes;
static uint16_t *stripe[2];
static uint16_t *palette;
static uint8_t trace[BINS];            /* last run's peak codes, lowest RF first */
/* Spectrum line, code * 16: the median of the last three run peaks, then
 * smoothed (rise 1/2, decay 1/4 per run). A packet seen in one run only
 * stays in the waterfall; the mean over frames would bury Wi-Fi entirely. */
static uint16_t *smooth;
static uint8_t *recent;                /* last three peak traces, display order */
static unsigned recent_head;
static bool have_trace;
/* Static data must end below the RF ring (sram_guard.ld): larger tables live
 * on the heap. The history takes whatever heap remains once the receiver is
 * up, row by row because that heap is fragmented; stored rows are stretched
 * over the WF_H display rows. */
static uint8_t *waterfall[WF_H];
#define WF_COLS (BINS / 2)             /* each stored column: the louder of two bins */
static unsigned wf_alloc, wf_head, wf_rows;
#define WF_HEAP_RESERVE 4096u         /* IQS output buffer and driver slack */
static unsigned view_mhz, view_rate;
static int bottom_db = 1000;           /* noise-tracked dBFS for the waterfall colors; 1000: unset */
static uint8_t (*canvas)[W / 8];       /* text rows, 1 bit per pixel */
static uint8_t (*scale)[SCALE_W / 8];  /* dBFS labels over the trace */
static uint8_t *col_lo, *col_hi;       /* trace rows per column */
static bool grid_row[SP_H];
static bool chan_col[W];               /* Wi-Fi channel centers */
static unsigned view_bins, view_first; /* displayed bins: a centered crop */

/* Byte-swapped RGB565 for the SPI byte order. */
static uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    uint16_t c = (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
    return (uint16_t)(c >> 8 | c << 8);
}
/* 16 blend levels from background to ink, for the 4-bit alpha font. */
static void ramp(uint16_t *out, const uint8_t bg[3], const uint8_t ink[3]) {
    for (unsigned a = 0; a < 16; a++)
        out[a] = rgb((bg[0] * (15 - a) + ink[0] * a) / 15, (bg[1] * (15 - a) + ink[1] * a) / 15,
                     (bg[2] * (15 - a) + ink[2] * a) / 15);
}

/* 5x8 column font, ASCII 0x20..0x7a, bit 0 at the top. */
static const uint8_t font[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},{0x2a,0x1c,0x7f,0x1c,0x2a},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4d,0x33},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1e},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},
    {0x3e,0x41,0x5d,0x59,0x4e},{0x7c,0x12,0x11,0x12,0x7c},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x41,0x3e},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x41,0x51,0x73},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x1c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7f,0x01,0x03},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4d,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x03,0x07,0x08,0x00},{0x20,0x54,0x54,0x78,0x40},{0x7f,0x28,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x28},
    {0x38,0x44,0x44,0x28,0x7f},{0x38,0x54,0x54,0x54,0x18},{0x00,0x08,0x7e,0x09,0x02},{0x18,0xa4,0xa4,0x9c,0x78},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x40,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x78,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0xfc,0x18,0x24,0x24,0x18},{0x18,0x24,0x24,0x18,0xfc},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x24},
    {0x04,0x04,0x3f,0x44,0x24},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x4c,0x90,0x90,0x90,0x7c},{0x44,0x64,0x54,0x4c,0x44},
};

/* Draw text with its top at row y of a 1-bit plane of the given byte width.
 * scale 2 doubles each pixel; x < 0 right-aligns at -x. */
static void glyphs(uint8_t *plane, unsigned stride, unsigned rows, int y, int x, const char *s, int scale) {
    int width = 6 * scale * (int)strlen(s);
    if (x < 0) x = -x - width + 1;
    if (x < 0) x = 0;
    for (; *s; s++, x += 6 * scale) {
        unsigned c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7a) c = '?';
        for (int col = 0; col < 5 * scale; col++) {
            int px = x + col;
            if (px >= (int)stride * 8) return;
            for (int row = 0; row < 8 * scale; row++) {
                int py = y + row;
                if (py < 0 || py >= (int)rows || !(font[c - 0x20][col / scale] >> (row / scale) & 1)) continue;
                plane[py * stride + (px >> 3)] |= (uint8_t)(1u << (px & 7));
            }
        }
    }
}
static void text(int y, int x, const char *s, int scale) { glyphs(&canvas[0][0], W / 8, TEXT_ROWS, y, x, s, scale); }

static void build_palette(void) {
    static const uint8_t stops[][3] = {{0,0,0},{0,0,140},{0,150,255},{0,255,120},{255,230,0},{255,40,0}};
    const unsigned segments = sizeof(stops) / sizeof(stops[0]) - 1;
    for (unsigned i = 0; i < 256; i++) {
        unsigned pos = i * segments, s = pos >> 8, f = pos & 255u; /* 1/256 segment units */
        if (s >= segments) { s = segments - 1; f = 255; }
        unsigned c[3];
        for (unsigned k = 0; k < 3; k++) c[k] = (stops[s][k] * (255u - f) + stops[s + 1][k] * f) / 255u;
        palette[i] = rgb(c[0], c[1], c[2]);
    }
}

/* ---- capture hooks ------------------------------------------------------ */

void lcd_view_capture_begin(void) {
    memset(acc, 0, BINS * sizeof(*acc));
    acc_frames = 0;
    frame_pos = 0;
    ring_local_sink = view_sink;
}

void lcd_view_capture_end(unsigned mhz, unsigned rate_hz) {
    ring_local_sink = NULL;
    bool fresh = mhz != view_mhz || rate_hz != view_rate;
    if (fresh) {
        /* New bins: restart the line and the history. */
        view_mhz = mhz;
        view_rate = rate_hz;
        wf_rows = 0;
        have_trace = false;
    }
    if (!acc_frames) return;
    /* Natural FFT order puts 0 Hz first, and on S3 RF above the LO lands at
     * negative baseband frequency: bin b shows LO - b * fs / N. */
    for (unsigned b = 0; b < BINS; b++) trace[(BINS + BINS / 2 - b) % BINS] = acc[b];
    if (!have_trace) for (unsigned k = 0; k < 3; k++) memcpy(recent + k * BINS, trace, BINS);
    recent_head = (recent_head + 1) % 3;
    memcpy(recent + recent_head * BINS, trace, BINS);
    for (unsigned d = 0; d < BINS; d++) {
        int a = recent[d], b = recent[BINS + d], c = recent[2 * BINS + d];
        int med = a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
        int t = med * 16, m = have_trace ? smooth[d] : t;
        smooth[d] = (uint16_t)(m + (t - m) / (t > m ? 2 : 4));
    }
    static bool wf_sized;
    if (!wf_sized) {
        wf_sized = true;
        while (wf_alloc < WF_H &&
               heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) > WF_HEAP_RESERVE + WF_COLS &&
               (waterfall[wf_alloc] = heap_caps_malloc(WF_COLS, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)))
            wf_alloc++;
    }
    if (wf_alloc) {
        wf_head = (wf_head + 1) % wf_alloc;
        for (unsigned c = 0; c < WF_COLS; c++)
            waterfall[wf_head][c] = trace[2 * c] > trace[2 * c + 1] ? trace[2 * c] : trace[2 * c + 1];
        if (wf_rows < wf_alloc) wf_rows++;
    }
    have_trace = true;
}

/* LCDDUMP? reply: LO, rate, frames of the last run, waterfall rows, the dBFS
 * at the trace bottom, then 256 hex bytes each of the run peaks (natural FFT
 * order) and the drawn line (display order, codes). */
size_t lcd_view_dump(char *out, size_t size) {
    static const char hex[] = "0123456789abcdef";
    size_t n = (size_t)snprintf(out, size, "LCDDUMP %u %u %u %u %d ", view_mhz, view_rate, (unsigned)acc_frames,
                                wf_alloc, bottom_db);
    for (unsigned part = 0; part < 2; part++) {
        for (unsigned b = 0; b < BINS && n + 3 < size; b++) {
            unsigned v = part == 0 ? acc[b] : smooth[b] / 16u;
            out[n++] = hex[v >> 4];
            out[n++] = hex[v & 15];
        }
        if (n + 2 < size) out[n++] = part < 1 ? ' ' : '\n';
    }
    out[n] = 0;
    return n;
}

/* ---- drawing ------------------------------------------------------------ */

static unsigned selected_mode;
static char box_value[LCD_MODES][8];   /* large values, drawn with lcd_font */

/* Blend the large values of the mode boxes into header row y. */
static void draw_values(int y, uint16_t *o) {
    static uint16_t levels[2][16];
    if (!levels[0][15]) {
        static const uint8_t bar[3] = {24, 28, 40}, text[3] = {230, 235, 245};
        static const uint8_t sel[3] = {255, 190, 70}, sel_text[3] = {10, 12, 20};
        ramp(levels[0], bar, text);
        ramp(levels[1], sel, sel_text);
    }
    const int row = y - VAL_Y;
    if (row < 0 || row >= LCD_FONT_H) return;
    for (unsigned m = 0; m < LCD_MODES; m++) {
        const uint16_t *lv = levels[m == selected_mode];
        int x = (int)m * BOX_W + 3;
        for (const char *c = box_value[m]; *c; c++) {
            const char *at = strchr(LCD_FONT_CHARS, *c);
            if (!at) continue;
            const lcd_font_glyph_t *g = &lcd_font_glyphs[at - LCD_FONT_CHARS];
            const uint8_t *bits = lcd_font_data + g->offset + row * ((g->width + 1) / 2);
            for (int gx = 0; gx < g->width && x + gx < (int)(m + 1) * BOX_W; gx++) {
                unsigned a = gx & 1 ? bits[gx / 2] & 15u : bits[gx / 2] >> 4;
                if (a) o[x + gx] = lv[a];
            }
            x += g->width;
        }
    }
}

static void compose_row(int y, uint16_t *o) {
    static uint16_t c_bar, c_text, c_sel, c_sel_text, c_edge, c_chan, c_chan_dim, c_bg, c_grid, c_lo, c_trace,
        c_fill, c_scale;
    if (!c_bar) {
        c_bar = rgb(24, 28, 40); c_text = rgb(230, 235, 245); c_bg = rgb(6, 10, 22);
        c_sel = rgb(255, 190, 70); c_sel_text = rgb(10, 12, 20); c_edge = rgb(70, 80, 100);
        c_chan = rgb(255, 190, 70); c_chan_dim = rgb(110, 85, 40); c_scale = rgb(150, 160, 180);
        c_grid = rgb(40, 52, 76); c_lo = rgb(90, 70, 40); c_trace = rgb(120, 255, 160); c_fill = rgb(20, 70, 50);
    }
    if (y < SP_Y) {
        const uint8_t *t = canvas[y];
        for (int x = 0; x < W; x++) {
            bool sel = (unsigned)(x / BOX_W) == selected_mode, ink = t[x >> 3] >> (x & 7) & 1;
            o[x] = x % BOX_W == 0 && x ? c_edge : sel ? (ink ? c_sel_text : c_sel) : (ink ? c_text : c_bar);
        }
        draw_values(y, o);
    } else if (y >= AX_Y && y < WF_Y) {
        const uint8_t *t = canvas[HDR_H + y - AX_Y];
        const uint16_t ink = y < AX_Y + 12 ? c_chan : c_text;
        for (int x = 0; x < W; x++) o[x] = t[x >> 3] >> (x & 7) & 1 ? ink : c_bar;
    } else if (y < AX_Y) {
        int r = y - SP_Y;
        for (int x = 0; x < W; x++) {
            uint16_t c = grid_row[r] || (x && x % (W / 4) == 0) ? c_grid : c_bg;
            if (x == W / 2) c = c_lo;
            else if (chan_col[x] && !(r & 3)) c = c_chan_dim;
            if (have_trace) {
                if (r >= col_lo[x] && r <= col_hi[x]) c = c_trace;
                else if (r > col_hi[x] && c == c_bg) c = c_fill;
            }
            if (x < SCALE_W && scale[r][x >> 3] >> (x & 7) & 1) c = c_scale;
            o[x] = c;
        }
    } else {
        unsigned r = (unsigned)(y - WF_Y) * wf_alloc / WF_H;
        if (r >= wf_rows) { memset(o, 0, W * sizeof(*o)); return; }
        const uint8_t *row = waterfall[(wf_head + wf_alloc - r) % wf_alloc];
        const int low_x10 = (bottom_db + WF_LOW_DB) * 10;
        for (int x = 0; x < W; x++) {
            int v = (CODE_DBFS_X10(row[(view_first + x * view_bins / W) / 2]) - low_x10) * 255 / (WF_RANGE_DB * 10);
            o[x] = palette[v < 0 ? 0 : v > 255 ? 255 : v];
        }
    }
}

static bool stripe_sent(void) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(free_stripes, &woken);
    return woken == pdTRUE;
}

/* Wi-Fi 20 MHz channel numbers in ascending frequency: 2.4 GHz 1-14, then
 * 5 GHz 32-144 and 149-177. Returns 0 after the last one. */
static unsigned next_channel(unsigned ch) {
    if (ch < 14) return ch + 1;
    if (ch == 14) return 32;
    if (ch < 144) return ch + 4;
    if (ch == 144) return 149;
    return ch < 177 ? ch + 4 : 0;
}
static unsigned channel_mhz(unsigned ch) {
    return ch == 14 ? 2484 : ch < 14 ? 2407 + 5 * ch : 5000 + 5 * ch;
}

/* Channel numbers (first axis row, with a tick) at their x positions. */
static unsigned label_channels(unsigned mhz, unsigned span) {
    unsigned at_lo = 0;
    int last_end = -100;
    memset(chan_col, 0, sizeof(chan_col));
    for (unsigned ch = 1; ch; ch = next_channel(ch)) {
        int offset = (int)channel_mhz(ch) - (int)mhz;
        if (!offset) at_lo = ch;
        if (2 * abs(offset) >= (int)span) continue;
        int x = W / 2 + offset * W / (int)span;
        chan_col[x] = true;
        canvas[HDR_H][x >> 3] |= (uint8_t)(1u << (x & 7));
        canvas[HDR_H + 1][x >> 3] |= (uint8_t)(1u << (x & 7));
        char s[4];
        snprintf(s, sizeof(s), "%u", ch);
        int start = x - 3 * (int)strlen(s) + 1;
        if (start < last_end + 3) continue; /* crowded: keep the tick only */
        text(HDR_H + 2, start, s, 1);
        last_end = start + 6 * (int)strlen(s);
    }
    return at_lo;
}

/* A battery at the right of the GAIN box: outline, terminal nub and a bar
 * to the charge, with the percentage inside, inverted where it crosses the
 * bar. */
#define BAT_W 29
#define BAT_H 14
#define BAT_NUB 2
#define BAT_X (W - 4 - BAT_W - BAT_NUB)
#define BAT_Y ((HDR_H - BAT_H) / 2)
static void draw_battery(int percent, bool charging) {
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    const int fill = (BAT_W - 4) * percent / 100;
    for (int y = BAT_Y; y < BAT_Y + BAT_H; y++)
        for (int dx = 0; dx < BAT_W + BAT_NUB; dx++) {
            bool edge = dx < BAT_W && (y == BAT_Y || y == BAT_Y + BAT_H - 1 || dx == 0 || dx == BAT_W - 1);
            bool nub = dx >= BAT_W && y >= BAT_Y + 4 && y < BAT_Y + BAT_H - 4;
            bool level = dx >= 2 && dx < 2 + fill && y >= BAT_Y + 2 && y < BAT_Y + BAT_H - 2;
            int px = BAT_X + dx;
            if (edge || nub || level) canvas[y][px >> 3] |= (uint8_t)(1u << (px & 7));
        }
    char s[16];
    snprintf(s, sizeof(s), "%s%d%%", charging ? "+" : "", percent);
    const int width = 6 * (int)strlen(s) - 1, x0 = BAT_X + (BAT_W - width) / 2, y0 = BAT_Y + (BAT_H - 7) / 2;
    for (const char *c = s; *c; c++)
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 8; row++) {
                if (!(font[*c - 0x20][col] >> row & 1)) continue;
                int px = x0 + (int)(c - s) * 6 + col;
                canvas[y0 + row][px >> 3] ^= (uint8_t)(1u << (px & 7));
            }
}

/* Waterfall colors follow the noise: the median of the displayed line in
 * 10 dB steps, with 4 dB of hysteresis, sits 20 dB above bottom_db. */
static void place_scale(void) {
    unsigned hist[256] = {0}, m = 0;
    for (unsigned d = 0; d < view_bins; d++) hist[smooth[view_first + d] / 16]++;
    for (unsigned seen = 0; m < 255 && (seen += hist[m]) < view_bins / 2; m++) {}
    int median_x10 = CODE_DBFS_X10(m), want = (median_x10 >= 0 ? median_x10 / 100 : -((-median_x10 + 99) / 100)) * 10 - 20;
    if (bottom_db == 1000 || median_x10 < (bottom_db + 10) * 10 - 40 || median_x10 > (bottom_db + 30) * 10 + 40) bottom_db = want;
}

void lcd_view_draw(const lcd_view_info_t *info) {
    selected_mode = info->mode;
    unsigned rate_mhz = info->rate_hz / 1000000u, span = info->span_mhz;
    view_bins = rate_mhz && span < rate_mhz ? BINS * span / rate_mhz : BINS;
    view_first = (BINS - view_bins) / 2;
    if (have_trace) place_scale();

    /* Fixed dBFS scale, labelled at the left every SP_GRID_DB. */
    const int range = SP_TOP_DB - SP_BOTTOM_DB;
    memset(grid_row, 0, sizeof(grid_row));
    memset(scale, 0, SP_H * sizeof(*scale));
    for (int g = range; g > 0; g -= SP_GRID_DB) { /* the bottom line has no room below it */
        int gy = SP_H - 1 - g * (SP_H - 1) / range;
        if (g < range) grid_row[gy] = true;
        char s[16];
        snprintf(s, sizeof(s), g == range ? "%d dBFS" : "%d", SP_BOTTOM_DB + g);
        glyphs(&scale[0][0], SCALE_W / 8, SP_H, gy + 2, 1, s, 1);
    }
    int prev = 0;
    for (int x = 0; x < W; x++) {
        int db_x10 = CODE_DBFS_X10(smooth[view_first + x * view_bins / W] / 16u);
        int y = SP_H - 1 - (db_x10 - SP_BOTTOM_DB * 10) * (SP_H - 1) / (range * 10);
        y = y < 0 ? 0 : y >= SP_H ? SP_H - 1 : y;
        if (!x) prev = y;
        col_lo[x] = (uint8_t)(y < prev ? y : prev);
        col_hi[x] = (uint8_t)(y > prev ? y : prev);
        prev = y;
    }

    memset(canvas, 0, TEXT_ROWS * sizeof(*canvas));
    char s[16];
    if (!info->rate_hz) goto send; /* blank frame */
    unsigned ch = label_channels(info->mhz, span);
    static const char *const labels[LCD_MODES] = {"CENTER", "SPAN MHz", "STEP MHz", "GAIN"};
    const unsigned values[LCD_MODES] = {info->mhz, span, info->step_mhz, info->gain};
    for (unsigned m = 0; m < LCD_MODES; m++) {
        text(1, (int)m * BOX_W + 3, labels[m], 1);
        snprintf(box_value[m], sizeof(box_value[m]), "%u", values[m]);
    }
    if (ch) {
        snprintf(s, sizeof(s), "ch%u", ch);
        text(1, -(BOX_W - 3), s, 1);
    }
    if (info->status) {
        snprintf(s, sizeof(s), "ERR%u", (unsigned)info->status);
        text(1, -(W - 3), s, 1);
    } else if (info->battery >= 0) {
        draw_battery(info->battery, info->charging);
    }
    snprintf(s, sizeof(s), "%u", info->mhz - span / 2);
    text(HDR_H + 14, 0, s, 1);
    snprintf(s, sizeof(s), "%u", info->mhz);
    text(HDR_H + 14, W / 2 - 3 * (int)strlen(s), s, 1);
    snprintf(s, sizeof(s), "%u", info->mhz + span / 2);
    text(HDR_H + 14, -(W - 1), s, 1);
send:

    for (int y0 = 0, cur = 0; y0 < H; y0 += STRIPE, cur ^= 1) {
        xSemaphoreTake(free_stripes, portMAX_DELAY);
        for (int r = 0; r < STRIPE; r++) compose_row(y0 + r, stripe[cur] + r * W);
        lcd_board_draw(y0, y0 + STRIPE, stripe[cur]);
    }
    /* The next capture masks interrupts: let both transfers complete first. */
    xSemaphoreTake(free_stripes, portMAX_DELAY);
    xSemaphoreTake(free_stripes, portMAX_DELAY);
    xSemaphoreGive(free_stripes);
    xSemaphoreGive(free_stripes);
}

/* ---- input -------------------------------------------------------------- */

/* Touch boards: a mode box selects its setting; elsewhere, including a
 * touch strip below the panel (CoreS3: y 240-279), the left third lowers,
 * the middle cycles and the right third raises, like the buttons. */
lcd_key_t lcd_view_key_at(int x, int y) {
    if (x < 0 || x >= W || y < 0) return LCD_KEY_NONE;
    if (y < HDR_H) return (lcd_key_t)(LCD_KEY_SELECT + x / BOX_W);
    return x < W / 3 ? LCD_KEY_PREV : x >= 2 * W / 3 ? LCD_KEY_NEXT : LCD_KEY_ENTER;
}

lcd_key_t lcd_view_key(bool *repeat) {
    static lcd_key_t held;
    static int64_t repeat_at;
    *repeat = false;
    lcd_key_t k = lcd_board_read_key();
    /* Releasing a ladder button sweeps it through the other ranges. */
    vTaskDelay(pdMS_TO_TICKS(2));
    if (lcd_board_read_key() != k) return LCD_KEY_NONE;
    int64_t now = esp_timer_get_time();
    if (k != held) {
        held = k;
        repeat_at = now + 500000;
        return k;
    }
    if ((k == LCD_KEY_PREV || k == LCD_KEY_NEXT) && now >= repeat_at) {
        repeat_at = now + 150000;
        *repeat = true;
        return k;
    }
    return LCD_KEY_NONE;
}

/* ---- setup -------------------------------------------------------------- */

void lcd_view_init(void) {
    acc = heap_caps_malloc(BINS * sizeof(*acc), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    scale = heap_caps_malloc(SP_H * sizeof(*scale), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    smooth = heap_caps_calloc(BINS, sizeof(*smooth), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    palette = heap_caps_malloc(256 * sizeof(*palette), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    canvas = heap_caps_malloc(TEXT_ROWS * sizeof(*canvas), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    col_lo = heap_caps_malloc(2 * W, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    recent = heap_caps_malloc(3 * BINS, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(acc && scale && recent && smooth && palette && canvas && col_lo ? ESP_OK : ESP_ERR_NO_MEM);
    col_hi = col_lo + W;
    build_palette();
    free_stripes = xSemaphoreCreateCounting(2, 2);
    for (int i = 0; i < 2; i++) stripe[i] = heap_caps_malloc(W * STRIPE * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(free_stripes && stripe[0] && stripe[1] ? ESP_OK : ESP_ERR_NO_MEM);

    lcd_board_init(STRIPE, stripe_sent);

    /* Blank the panel before lighting the backlight. */
    const lcd_view_info_t idle = {.mhz = 0, .rate_hz = 0};
    lcd_view_draw(&idle);
    lcd_board_backlight(true);
    /* Keep these pins out of the host GPIO control set. */
    esp_gpio_reserve(lcd_board_pins());
}
