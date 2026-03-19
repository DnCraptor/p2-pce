/**
 * VGA Driver - based on pico-286's vga-nextgen by xrip.
 * Reads directly from emulator VRAM and renders text/graphics on-the-fly.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#pragma GCC optimize("Ofast")

#include "vga_hw.h"
#include "vga_osd.h"
#include "font8x16.h"
#include "debug.h"
#include "board_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"
#include "pico/time.h"
#include <arm_acle.h>
#include "../../drivers/psram/psram_init.h"

bool SELECT_VGA = false;
extern bool required_to_repair_text_pal;

// ============================================================================
// PIO Program
// ============================================================================

static const uint16_t pio_vga_instructions[] = {
    0x6008,  // out pins, 8
};

static const struct pio_program pio_vga_program = {
    .instructions = pio_vga_instructions,
    .length = 1,
    .origin = -1,
};

// ============================================================================
// VGA Timing (640x480 @ 60Hz - standard? VGA text mode timing)
// ============================================================================
#ifndef VGA_SHIFT_PICTURE
#define VGA_SHIFT_PICTURE 144
#endif

#define VGA_CLK 25175000.0f

#define LINE_SIZE       800
#define N_LINES_TOTAL   525
#define N_LINES_VISIBLE 480
#define LINE_VS_BEGIN   490
#define LINE_VS_END     491

// Default active area for 400-line modes (text, CGA, EGA ≤400, mode 13h)
#define DEFAULT_ACTIVE_START  40
#define DEFAULT_ACTIVE_END    (DEFAULT_ACTIVE_START + 400)  // 440

// Dynamic active area — adjusted by vga_hw_set_gfx_mode() for 480-line modes
int active_start = DEFAULT_ACTIVE_START;
int active_end   = DEFAULT_ACTIVE_END;

#define HS_SIZE             96
#define SHIFT_PICTURE       VGA_SHIFT_PICTURE  // Where active video starts (from board_config.h)

// Sync encoding in bits 6-7
#define TMPL_LINE           0xC0
#define TMPL_HS             0x80
#define TMPL_VS             0x40
#define TMPL_VHS            0x00

// ============================================================================
// Module State
// ============================================================================

// Line pattern buffers - now 6 buffers:
// 0 = hsync template, 1 = vsync template, 2-5 = active video (4 line rolling buffer)
static uint32_t *lines_pattern[6];
static uint32_t *lines_pattern_data = NULL;

// DMA channels
static int dma_data_chan = -1;
static int dma_ctrl_chan = -1;

// PIO state
static uint vga_sm = 0;

// Text buffer in SRAM (non-static to allow OSD reuse when paused)
uint8_t text_buffer_sram[80 * 25 * 2] __attribute__((aligned(4)));
static volatile int update_requested = 0;  // Set by update call

#define GFX_BUFFER_SIZE (256 * 1024)
uint8_t gfx_buffer[GFX_BUFFER_SIZE] __attribute__((aligned(4)));

// Fast text palette for 2-bit pixel pairs
extern uint32_t conv_color2[1024]; // 4096 in hdmi only
static uint16_t* txt_palette_fast = (uint16_t*)&conv_color2[0]; ///[256 * 4]; 2048, reusing

// Graphics palette (256 entries) - 16-bit dithered format
// Each entry: low byte = c_hi (conv0), high byte = c_lo (conv1)
// even - A, odd - B
uint16_t palette_a[512] __aligned(4);
static uint16_t* palette_b = &palette_a[256];

// 16-color EGA palette (for 16-color modes) - 8-bit VGA format with sync bits
static uint8_t ega_palette[16];

// CGA 4-color palette - 8-bit VGA format with sync bits
// Default to palette 1 high intensity: black, cyan, magenta, white
static uint8_t cga_palette[4];

// Current video mode (0=blank, 1=text, 2=graphics)
int current_mode = 1;  // Default text mode

// Graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
int gfx_submode = 3;
int gfx_width = 320;
int gfx_height = 200;
int gfx_line_offset = 40;  // Words per line (40 for 320px EGA, 80 for 640px)
int gfx_sram_stride = 41;  // Words per line in SRAM buffer (width/8 + 1)

// Cursor state
int cursor_x = 0, cursor_y = 0;
int cursor_start = 0, cursor_end = 15;
int cursor_blink_state = 1;

// Per-frame values latched by ISR from vga_state->cr[] late in vblank
uint16_t frame_vram_offset   = 0;
uint8_t  frame_pixel_panning = 0;
int      frame_line_compare  = -1;

int text_cols = 80;
// Stride in *character cells* (uint32_t per cell in gfx_buffer text layout).
// For VGA CRTC Offset (0x13): cells_per_row = cr13 * 2 (80-col -> 40*2, 40-col -> 20*2).
int text_stride_cells = 80;

inline static void vga_hw_submit_text_geom(int cols, int stride_cells) {
    if (cols != 40 && cols != 80)
        return;
    if (stride_cells <= 0 || stride_cells > 256)
        return;
    text_cols = cols;
    text_stride_cells = stride_cells;
}

// ============================================================================
// Color Conversion
// ============================================================================

// Dithering lookup tables from quakegeneric
// These map 3-bit values (0-7) to 2-bit values with different rounding
// conv0 rounds down more, conv1 rounds up more
// Alternating between them spatially creates perceived intermediate colors
static const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
static const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

// Convert 6-bit VGA DAC values (0-63) to 8-bit output with sync bits
// Used for EGA/CGA palettes (no dithering)
static inline uint8_t vga_color_to_output(uint8_t r6, uint8_t g6, uint8_t b6) {
    // Map 6-bit VGA colors to 2-bit per channel (RRGGBB in bits 0-5)
    // Bits 6-7 are sync: 0xC0 = no sync pulses during active video
    uint8_t r2 = r6 >> 4;  // 6-bit to 2-bit
    uint8_t g2 = g6 >> 4;
    uint8_t b2 = b6 >> 4;
    return TMPL_LINE | (r2 << 4) | (g2 << 2) | b2;
}

static inline int c6_to_8(int v)
{
    v &= 0x3f;
    int b = v & 1;
    return (v << 2) | (b << 1) | b;
}

void graphics_set_palette_hdmi(const uint8_t R, const uint8_t G, const uint8_t B,  uint8_t i);
void graphics_set_palette_hdmi2(
    const uint8_t R1, const uint8_t G1, const uint8_t B1,
    const uint8_t R2, const uint8_t G2, const uint8_t B2,
    uint8_t i
);

// Convert 6-bit VGA DAC values to 16-bit dithered output
// Returns: low byte = c_hi (conv0), high byte = c_lo (conv1)
// When output as 16-bit, adjacent pixels get different colors for spatial dithering
static void vga_color_to_dithered(uint8_t r6, uint8_t g6, uint8_t b6, uint32_t idx) {
    if (!SELECT_VGA) {
        graphics_set_palette_hdmi(c6_to_8(r6), c6_to_8(g6), c6_to_8(b6), idx);
        return;
    }
    // Convert 6-bit (0-63) to 3-bit (0-7) for dither table lookup
    // 63/7 ≈ 9, so divide by 9
    uint8_t r = r6 / 9;
    uint8_t g = g6 / 9;
    uint8_t b = b6 / 9;
    if (r > 7) r = 7;
    if (g > 7) g = 7;
    if (b > 7) b = 7;

    uint8_t c_hi = TMPL_LINE | (conv0[r] << 4) | (conv0[g] << 2) | conv0[b];
    uint8_t c_lo = TMPL_LINE | (conv1[r] << 4) | (conv1[g] << 2) | conv1[b];

    palette_a[idx] = (uint16_t)c_hi | ((uint16_t)c_lo << 8);
    palette_b[idx] = (uint16_t)c_lo | ((uint16_t)c_hi << 8);
}

static void init_palettes(void) {
    // Standard 16-color text palette (CGA colors)
    // Each entry is 6-bit: RRGGBB
    static const uint8_t cga_colors[16] = {
        0x00 | TMPL_LINE,  // 0: Black
        0x02 | TMPL_LINE,  // 1: Blue
        0x08 | TMPL_LINE,  // 2: Green
        0x0A | TMPL_LINE,  // 3: Cyan
        0x20 | TMPL_LINE,  // 4: Red
        0x22 | TMPL_LINE,  // 5: Magenta
        0x28 | TMPL_LINE,  // 6: Brown (dark yellow)
        0x2A | TMPL_LINE,  // 7: Light Gray
        0x15 | TMPL_LINE,  // 8: Dark Gray
        0x17 | TMPL_LINE,  // 9: Light Blue
        0x1D | TMPL_LINE,  // 10: Light Green
        0x1F | TMPL_LINE,  // 11: Light Cyan
        0x35 | TMPL_LINE,  // 12: Light Red
        0x37 | TMPL_LINE,  // 13: Light Magenta
        0x3D | TMPL_LINE,  // 14: Yellow
        0x3F | TMPL_LINE,  // 15: White
    };
    
    // Build fast palette for text rendering
    // Each entry handles 2 pixels (foreground/background combinations)
    // For 2-bit value from glyph: high bit is LEFT pixel, low bit is RIGHT pixel
    // When we extract (glyph >> 6) & 3, we get bits 7,6 where bit7=left, bit6=right
    // Index XY (X=bit7, Y=bit6): X is left pixel, Y is right pixel
    // Output 16-bit: low byte outputs first (left), high byte outputs second (right)
    for (int i = 0; i < 256; i++) {
        uint8_t fg = cga_colors[i & 0x0F];
        uint8_t bg = cga_colors[i >> 4];
        
        // Index bits: [left_pixel][right_pixel]
        // For little-endian 16-bit output: low byte = left, high byte = right
        txt_palette_fast[i * 4 + 0] = bg | (bg << 8);  // 00: left=bg, right=bg
        txt_palette_fast[i * 4 + 1] = fg | (bg << 8);  // 01: left=fg, right=bg (bit7=0,bit6=1)
        txt_palette_fast[i * 4 + 2] = bg | (fg << 8);  // 10: left=bg, right=fg (bit7=1,bit6=0)
        txt_palette_fast[i * 4 + 3] = fg | (fg << 8);  // 11: left=fg, right=fg
    }
    
    // Initialize 256-color dithered palette with black (will be overwritten by emulator)
    for (int i = 0; i < 256; i++) {
        vga_color_to_dithered(0, 0, 0, i);
    }
    
    // Initialize CGA 4-color palette (palette 1 high intensity: black, cyan, magenta, white)
    // Use direct RGB values for proper CGA colors
    cga_palette[0] = vga_color_to_output(0, 0, 0);     // Black
    cga_palette[1] = vga_color_to_output(0, 63, 63);   // Cyan (bright)
    cga_palette[2] = vga_color_to_output(63, 0, 63);   // Magenta (bright)
    cga_palette[3] = vga_color_to_output(63, 63, 63);  // White
}

// ============================================================================
// DMA Interrupt Handler - Renders each scanline
// ============================================================================

// Dispatch to appropriate renderer based on current mode
static void __time_critical_func(render_line)(uint32_t line, uint32_t *output_buffer) {
    // --- Верхнее поле ---
    if (line < (uint32_t)active_start) {
        uint32_t blank = TMPL_LINE | (TMPL_LINE<<8) | (TMPL_LINE<<16) | (TMPL_LINE<<24);
        uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);
        for (int i = 0; i < 160; i++)
            out32[i] = blank;
        return;
    }

    // --- Нижнее поле ---
    if (line >= (uint32_t)active_end) {
        uint32_t blank = TMPL_LINE | (TMPL_LINE<<8) | (TMPL_LINE<<16) | (TMPL_LINE<<24);
        uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);
        for (int i = 0; i < 160; i++)
            out32[i] = blank;
        return;
    }

    // --- Активная зона 640×400 ---
    line -= active_start;
    // If OSD is visible, it takes over the display completely
    // (it reuses text_buffer_sram so we can't render normal text)
    if (osd_is_visible()) {
        osd_render_line_vga(line, output_buffer);
        return;
    }
    // mode 0 = blanked (AR bit5 cleared during BIOS mode transitions).
    // Emit black pixels with sync bits so the monitor sees a valid signal.
    uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);
    uint32_t blank = TMPL_LINE | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
    for (int i = 0; i < 160; i++) out32[i] = blank;
}

static inline void vga_hw_set_mode(int mode);

// ============================================================================
// Core 1 ISR load meter
// Displayed as a 5-pixel-tall bar in the inactive region BELOW the active
// picture (lines active_end .. N_LINES_VISIBLE-1, typically 440..479 = 40 px).
//   RED    = ISR busy time fraction of full frame
//   GREEN  = remaining budget
//   YELLOW = 4px marker at left edge when a missed/late ISR was detected
// 640 pixels wide = full frame budget.
// ============================================================================
#define LOAD_BAR_ENABLE  0   // set to 0 to disable
#define LOAD_BAR_HEIGHT  10   // 5px ISR load + 5px new_frame duration

static uint32_t isr_busy_us_acc  = 0;
static uint32_t isr_busy_us_prev = 0;
static uint32_t blank_frame_count = 0;
static uint32_t blank_frame_prev  = 0;
// Missed ISR: detected when current_line jumps by >1 between two calls
static uint32_t missed_isr_count = 0;
static uint32_t missed_isr_prev  = 0;
// Once any missed ISR or blank frame is detected, stay yellow forever
static uint32_t anomaly_ever_seen = 0;
// PIO TX stall detected (FDEBUG.TXSTALL) - confirmed sync loss
static uint32_t pio_stall_ever_seen = 0;
// Max observed vga_hw_new_frame duration in µs (for debugging long new_frame calls)
static uint32_t new_frame_max_us = 0;
// Deferred frame update flag (set in ISR, processed outside)
volatile uint32_t frame_update_request = 0;

// Frame period in µs — computed in vga_hw_init() to avoid overflow.
static uint32_t frame_period_us = 16688u;

// Render the load bar directly into a line buffer.
// Called for absolute scanlines in range [active_end .. active_end+LOAD_BAR_HEIGHT).
static void __time_critical_func(render_load_bar)(uint32_t abs_line,
                                                   uint32_t *output_buffer) {
#if LOAD_BAR_ENABLE
    if (abs_line < (uint32_t)active_end) return;
    if (abs_line >= (uint32_t)active_end + LOAD_BAR_HEIGHT) return;

    uint8_t *out = (uint8_t *)output_buffer + SHIFT_PICTURE;

    // Top 5 rows: ISR total load (red/green/yellow/blue as before)
    // Bottom 5 rows: vga_hw_new_frame() duration vs 32µs budget (orange/green)
    bool is_bottom = (abs_line >= (uint32_t)active_end + 5);

    if (!is_bottom) {
        // ISR load bar
        uint32_t busy  = isr_busy_us_prev;
        uint32_t total = frame_period_us ? frame_period_us : 16688u;
        if (busy > total) busy = total;
        uint32_t red_px = (busy * 640u) / total;

        uint8_t idle_color;
        if (pio_stall_ever_seen)
            idle_color = 0xC3u;  // blue
        else if (anomaly_ever_seen)
            idle_color = 0xFCu;  // yellow
        else
            idle_color = 0xCCu;  // green

        for (uint32_t x = 0; x < 640; x++)
            out[x] = (x < red_px) ? 0xF0u : idle_color;
    } else {
        // new_frame duration bar: budget = 32µs = one scanline
        // ORANGE pixel: R=3,G=1,B=0 → 0xC0|0x34 = 0xF4
        uint32_t nf = new_frame_max_us;
        uint32_t budget = 32u;
        if (nf > 640u) nf = 640u;  // cap visual at 640µs (20× budget)
        // Scale: 640px = 20 × budget; each px = 1µs
        uint32_t orange_px = (nf < 640u) ? nf : 640u;
        // Mark the budget boundary with a bright white tick
        for (uint32_t x = 0; x < 640; x++) {
            if (x == budget)
                out[x] = 0xFFu;  // white tick at 32µs mark
            else if (x < orange_px)
                out[x] = 0xF4u;  // orange: used new_frame time
            else
                out[x] = 0xCCu;  // green: spare
        }
    }
#endif
}

static void __isr __time_critical_func(dma_handler_vga)(void) {
    uint32_t t_enter = timer_hw->timerawl;
    dma_hw->ints0 = 1u << dma_ctrl_chan;
    static uint32_t current_line = 0;
    static uint32_t prev_line    = 0xFFFFFFFF;
    uint32_t line = current_line++;

    // Check PIO FDEBUG.TXSTALL - sticky bit set if PIO ever ran out of data
    // This is the definitive indicator of a DMA underrun causing sync loss.
    uint32_t txstall_bit = 1u << (8 + vga_sm);
    if (VGA_PIO->fdebug & txstall_bit) {
        VGA_PIO->fdebug = txstall_bit;  // clear (write 1 to clear)
        pio_stall_ever_seen = 1;
    }

    // Detect missed ISR: DMA advanced more than 1 line between two ISR calls
    if (prev_line != 0xFFFFFFFF && line > prev_line + 1)
        missed_isr_count += line - prev_line - 1;
    prev_line = line;

    if (line >= N_LINES_TOTAL) {
        line = current_line = prev_line = 0;
        isr_busy_us_prev  = isr_busy_us_acc;
        isr_busy_us_acc   = 0;
        blank_frame_prev  = blank_frame_count;
        blank_frame_count = 0;
        missed_isr_prev   = missed_isr_count;
        missed_isr_count  = 0;
        if (missed_isr_prev || blank_frame_prev)
            anomaly_ever_seen = 1;
        // Defer heavy frame work outside ISR
        frame_update_request = 1;
    }

    // Vertical blanking region
    if (line >= N_LINES_VISIBLE) {
        if (line >= LINE_VS_BEGIN && line <= LINE_VS_END) {
            dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[1], false);
        } else {
            dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[0], false);
        }

        // Line N_LINES_TOTAL-4 (521): late in vblank, just before DMA needs line 0.
        // Wolf3D has already written the new page address to CRTC by now.
        // Read cr[] and ar[] directly — no intermediate volatile copies.
        if (line == N_LINES_TOTAL - 4) {
            render_line(0, lines_pattern[2]);
            render_line(1, lines_pattern[3]);
            render_line(2, lines_pattern[4]);
            render_line(3, lines_pattern[5]);
        }
        return;
    }
    
    // Active video: DMA reads from buffer (line % 4), we render (line + 2) % 4
    uint32_t read_buf = 2 + (line & 3);
    uint32_t render_buf = 2 + ((line + 2) & 3);
    uint32_t render_line_num = line + 2;

    // Set DMA to read from the buffer we already rendered
    dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[read_buf], false);

    // Pre-render 2 lines ahead
    if (render_line_num < N_LINES_VISIBLE) {
        render_line(render_line_num, lines_pattern[render_buf]);
        // Load bar goes into the inactive region below active_end (e.g. lines 440-479)
        render_load_bar(render_line_num, lines_pattern[render_buf]);
    }

    // Accumulate ISR busy time (µs)
    isr_busy_us_acc += timer_hw->timerawl - t_enter;
}

// ============================================================================
// Public API
// ============================================================================
int testPins(uint32_t pin0, uint32_t pin1);
void graphics_init_hdmi();
// From main.c — needed for safe flash access at high clock speeds
extern void set_flash_timings(int cpu_mhz);

// HDMI TMDS requires an exact 252 MHz PIO clock. Boost clk_sys to 504 MHz
// so the PIO divider is an integer 2 (no jitter).
#define HDMI_SYS_CLOCK_MHZ 504

static void hdmi_boost_clock(void) {
    int cur_mhz = clock_get_hz(clk_sys) / 1000000;
    if (cur_mhz >= HDMI_SYS_CLOCK_MHZ) return;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_65);
    sleep_ms(50);
    set_flash_timings(HDMI_SYS_CLOCK_MHZ);
    set_sys_clock_khz(HDMI_SYS_CLOCK_MHZ * 1000, false);
    // Immediately reinit PSRAM for the new clock speed.
    // Without this, PSRAM runs at ~177 MHz (overclocked) until
    // reconfigure_clocks runs much later, causing intermittent hangs.
    psram_init_with_freq(get_psram_pin(), PSRAM_MAX_FREQ_MHZ);
}

/* Recalculate and apply VGA PIO clock divider after a sysclk change. */
void vga_hw_reclock(void) {
    if (!SELECT_VGA) return;
    float clk_div = (float)clock_get_hz(clk_sys) / VGA_CLK;
    uint32_t div_int  = (uint32_t)clk_div;
    uint32_t div_frac = (uint32_t)((clk_div - (float)div_int) * 256.0f);
    VGA_PIO->sm[vga_sm].clkdiv = (div_int << 16) | (div_frac << 8);
    frame_period_us = (uint32_t)((float)(LINE_SIZE * N_LINES_TOTAL) * 1000000.0f / VGA_CLK);
}

void vga_hw_init(void) {
    uint8_t linkVGA01 = testPins(VGA_BASE_PIN, VGA_BASE_PIN + 1);
    #if defined(BOARD_Z0) || defined(BOARD_Z2) || defined(BOARD_DV)
        SELECT_VGA = linkVGA01 == 0x1F;
    #else
        SELECT_VGA = (linkVGA01 == 0) || (linkVGA01 == 0x1F);
    #endif
    if (!SELECT_VGA) {
        hdmi_boost_clock();
        graphics_init_hdmi();
        return;
    }
    DBG_PRINT("VGA Init (pico-286 style)...\n");

    init_palettes();

    // Calculate clock divider
    float sys_clk = (float)clock_get_hz(clk_sys);
    float clk_div = sys_clk / VGA_CLK;

    // Frame period: LINE_SIZE pixels per line, N_LINES_TOTAL lines, at VGA_CLK px/s
    // Use float to avoid 32-bit overflow (800*525*1e6 ≈ 4.2e11)
    frame_period_us = (uint32_t)((float)(LINE_SIZE * N_LINES_TOTAL) * 1000000.0f / VGA_CLK);

    DBG_PRINT("  System clock: %.1f MHz\n", sys_clk / 1e6f);
    DBG_PRINT("  Clock divider: %.4f\n", clk_div);
    DBG_PRINT("  Frame period: %lu us\n", (unsigned long)frame_period_us);
    
    // Allocate line pattern buffers (6 buffers: 2 sync + 4 active)
    lines_pattern_data = (uint32_t *)calloc(LINE_SIZE * 6 / 4, sizeof(uint32_t));
    if (!lines_pattern_data) {
        printf("ERROR: Failed to allocate VGA buffers!\n");
        return;
    }
    
    for (int i = 0; i < 6; i++) {
        lines_pattern[i] = &lines_pattern_data[i * (LINE_SIZE / 4)];
    }
    
    // Initialize line templates
    uint8_t *base = (uint8_t *)lines_pattern[0];
    memset(base, TMPL_LINE, LINE_SIZE);
    memset(base, TMPL_HS, HS_SIZE);
    
    base = (uint8_t *)lines_pattern[1];
    memset(base, TMPL_VS, LINE_SIZE);
    memset(base, TMPL_VHS, HS_SIZE);
    
    // Initialize all 4 active line buffers with the sync template
    for (int i = 2; i < 6; i++) {
        memcpy(lines_pattern[i], lines_pattern[0], LINE_SIZE);
    }

    // Initialize PIO
    uint offset = pio_add_program(VGA_PIO, &pio_vga_program);
    vga_sm = pio_claim_unused_sm(VGA_PIO, true);
    
    // Configure GPIO pins
    for (int i = 0; i < 8; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(VGA_PIO, VGA_BASE_PIN + i);
        gpio_set_slew_rate(VGA_BASE_PIN + i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(VGA_BASE_PIN + i, GPIO_DRIVE_STRENGTH_8MA);
    }
    
    // Configure PIO state machine
    pio_sm_set_consecutive_pindirs(VGA_PIO, vga_sm, VGA_BASE_PIN, 8, true);
    
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    
    pio_sm_init(VGA_PIO, vga_sm, offset, &c);
    
    // Set clock divider (16.8 fixed point format: 16 bits integer, 8 bits fraction)
    // clk_div is a float, convert to 24.8 fixed point
    uint32_t div_int = (uint32_t)clk_div;
    uint32_t div_frac = (uint32_t)((clk_div - div_int) * 256.0f);
    uint32_t div_reg = (div_int << 16) | (div_frac << 8);
    VGA_PIO->sm[vga_sm].clkdiv = div_reg;
    
    DBG_PRINT("  Clock divider reg: 0x%08x (int=%d, frac=%d)\n", div_reg, div_int, div_frac);
    
    pio_sm_set_enabled(VGA_PIO, vga_sm, true);
    
    // Initialize DMA
    dma_data_chan = dma_claim_unused_channel(true);
    dma_ctrl_chan = dma_claim_unused_channel(true);
    
    // Data channel
    dma_channel_config c0 = dma_channel_get_default_config(dma_data_chan);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);
    
    uint dreq = (VGA_PIO == pio0) ? DREQ_PIO0_TX0 + vga_sm : DREQ_PIO1_TX0 + vga_sm;
    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, dma_ctrl_chan);
    
    dma_channel_configure(dma_data_chan, &c0, &VGA_PIO->txf[vga_sm],
                          lines_pattern[0], LINE_SIZE / 4, false);
    
    // Control channel
    dma_channel_config c1 = dma_channel_get_default_config(dma_ctrl_chan);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_chain_to(&c1, dma_data_chan);
    
    dma_channel_configure(dma_ctrl_chan, &c1,
                          &dma_hw->ch[dma_data_chan].read_addr,
                          &lines_pattern[0], 1, false);
    
    // Set up interrupt with highest priority to prevent preemption
    // VGA timing is critical - the ISR must run within ~32us (one scanline)
    // to update the DMA read address before the next transfer starts.
    // Priority 0x00 = highest priority on ARM Cortex-M.
    irq_set_exclusive_handler(VGA_DMA_IRQ, dma_handler_vga);
    irq_set_priority(VGA_DMA_IRQ, 0x00);
    dma_channel_set_irq0_enabled(dma_ctrl_chan, true);
    irq_set_enabled(VGA_DMA_IRQ, true);
    
    // Start DMA
    dma_start_channel_mask(1u << dma_data_chan);
    
    DBG_PRINT("  VGA started (640x400 text mode, IRQ priority=0x00)!\n");
}

static inline void vga_hw_set_mode(int mode) {
    // Ignore mode 0 (blank): this is a transient state the BIOS sets
    // while reprogramming registers during mode switches.  If we apply
    // it we permanently black out the display until the next reboot.
    // The render_line() fallback already outputs a valid blank scanline
    // for current_mode==0 so the monitor signal stays clean.
    if (mode == 0) return;
    if (mode == 1) {
        active_start = DEFAULT_ACTIVE_START;
        active_end = DEFAULT_ACTIVE_END;
    }
    current_mode = mode;
}

void __time_critical_func(vga_hw_set_cursor)(int x, int y, int start, int end, int char_height) {
    cursor_x = x;
    cursor_y = y;
    // Scale cursor scanlines from emulated char_height to our 16-line font
    // For example: if char_height=8 and cursor is at scanlines 6-7,
    // we scale to 12-15 for a 16-line font (preserving bottom position)
    if (char_height > 0 && char_height != 16) {
        cursor_start = start * 16 / char_height;
        cursor_end = (end + 1) * 16 / char_height - 1;
        if (cursor_end < cursor_start) cursor_end = cursor_start;
        if (cursor_end > 15) cursor_end = 15;
    } else {
        cursor_start = start;
        cursor_end = end;
    }
}

// These setters are no longer used — ISR reads VGA registers directly.
// Kept as stubs so any remaining callers still compile.
void vga_hw_set_vram_offset(uint16_t offset)  { (void)offset; }
void vga_hw_set_panning(uint8_t panning)       { (void)panning; }
void vga_hw_set_line_compare(int line)          { (void)line; }

// Update palette from emulator's 6-bit VGA DAC values
// palette_data is 768 bytes (256 entries × 3 bytes RGB, each 0-63)
// Uses dithering for ~2197 perceived colors from 64 actual colors
void vga_hw_set_palette(const uint8_t *palette_data) {
    for (int i = 0; i < 256; i++) {
        uint8_t r6 = palette_data[i * 3 + 0];
        uint8_t g6 = palette_data[i * 3 + 1];
        uint8_t b6 = palette_data[i * 3 + 2];
        vga_color_to_dithered(r6, g6, b6, i);
    }
}

// Update EGA 16-color palette from AC palette registers
// palette16_data is 48 bytes (16 entries × 3 bytes RGB, each 0-63)
void __time_critical_func(vga_hw_set_palette16)(const uint8_t *palette16_data) {
    for (int i = 0; i < 16; i++) {
        uint8_t r6 = palette16_data[i * 3 + 0];
        uint8_t g6 = palette16_data[i * 3 + 1];
        uint8_t b6 = palette16_data[i * 3 + 2];
        if (SELECT_VGA) {
            ega_palette[i] = vga_color_to_output(r6, g6, b6);
        } else {
            if (gfx_submode == 2) {
                for (int j = 0; j < 16; j++) {
                    uint8_t rj = palette16_data[j * 3 + 0];
                    uint8_t gj = palette16_data[j * 3 + 1];
                    uint8_t bj = palette16_data[j * 3 + 2];
                    graphics_set_palette_hdmi2(
                        c6_to_8(r6), c6_to_8(g6), c6_to_8(b6),
                        c6_to_8(rj), c6_to_8(gj), c6_to_8(bj),
                        (i << 4) | j
                    );
                }
            } else {
                graphics_set_palette_hdmi(c6_to_8(r6), c6_to_8(g6), c6_to_8(b6), i);
            }
        }
    }
}

// Set graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
void __time_critical_func(vga_hw_set_gfx_mode)(int submode, int width, int height, int line_offset) {
    gfx_submode = submode;
    gfx_width = width;
    gfx_height = height;
    gfx_line_offset = line_offset > 0 ? line_offset : (width / 8);
    gfx_sram_stride = (width / 8) + 1;

    // Adjust active display area based on mode requirements
    if (height > 400 || (submode == 5 && height > 200)) {
        // 480-line modes: mode 12h (height=480) or Mode X 320×240 (height=240, doubled)
        active_start = 0;
        active_end = N_LINES_VISIBLE;  // 480
    } else {
        active_start = DEFAULT_ACTIVE_START;
        active_end = DEFAULT_ACTIVE_END;
    }
}
