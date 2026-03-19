/**
 * Main entry point for the RP2350 platform.
 * Initializes hardware, loads configuration, and starts the emulator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/qmi.h"

#include "board_config.h"
#include "psram_init.h"
#include "vga_hw.h"
#include "ps2kbd_wrapper.h"
#include "ps2mouse.h"
#ifdef USB_HID_ENABLED
#include "usbkbd_wrapper.h"
#include "usbmouse_wrapper.h"
#endif
#include "sdcard.h"
#include "ff.h"
#include "audio.h"

#include "ini.h"
#include "debug.h"
#include "diskui.h"
#include "settingsui.h"
#include "config_save.h"
#include "vga_osd.h"

#if FEATURE_AUDIO_PWM
#include <hardware/pwm.h>
#endif

// Flag to track if VGA is initialized (for error display)
static volatile bool vga_initialized = false;
static volatile bool initialized = false;

// to call DMA wait not from ISR for timer
bool repeat_me_often(void);
// core1 timer
bool timer_callback(repeating_timer_t *rt);
static bool __not_in_flash_func(timer_callback0)(repeating_timer_t *rt) {
    timer_callback(rt);
    return true;
}

uint32_t conv_color[1224] __aligned(4096);
uint32_t conv_color2[1024]; // backup to fast restore pallete

// TODO: separate .c file
#if DVI_A
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#define DVI_TIMING dvi_timing_640x480p_60hz
struct dvi_inst dvi0;
#define AUDIO_BUFFER_SIZE   256
audio_sample_t      audio_buffer[AUDIO_BUFFER_SIZE];
struct repeating_timer audio_timer;

bool __not_in_flash_func(audio_timer_callback)(struct repeating_timer *t) {
	while(true) {
		int size = get_write_size(&dvi0.audio_ring, false);
		if (size == 0) return true;
		audio_sample_t *audio_ptr = get_write_pointer(&dvi0.audio_ring);
		audio_sample_t sample;
		static uint sample_count = 0;
		for (int cnt = 0; cnt < size; cnt++) {
			sample.channels[0] = 0; //commodore_argentina[sample_count % commodore_argentina_len] << 8;
			sample.channels[1] = 0; //commodore_argentina[(sample_count+1024) % commodore_argentina_len] << 8;
			*(audio_ptr++) = sample;
			sample_count = sample_count + 1;
		}
		increase_write_pointer(&dvi0.audio_ring, size);
	}
}

#define FRAME_WIDTH 640
#define FRAME_HEIGHT (480 / DVI_VERTICAL_REPEAT) // lines duplicates (see DVI_VERTICAL_REPEAT)

#include "font8x16.h"
extern uint8_t text_buffer_sram[80 * 25 * 2];
// Render OSD overlay onto a scanline
static void __not_in_flash_func(osd_render_line_dvi)(uint line, uint32_t *tmdsbuf) {
    // VGA output is 640x400, text mode is 80x25 with 8x16 font
    // So each character row is 16 scanlines
    uint32_t char_row = line >> 4;
    const uint8_t* glyph_line = font_8x16 + (line & 15);

    if (char_row >= OSD_ROWS) return;

    // Get pointer to this row in OSD buffer (reuses text_buffer_sram)
    uint8_t *row_data = &text_buffer_sram[char_row * OSD_COLS * 2];
    uint8_t *row_data_end = row_data + OSD_COLS * 2;

    uint32_t* targetR = tmdsbuf + 2 * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD);
    uint32_t* targetG = tmdsbuf + 1 * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD);
    uint32_t* targetB = tmdsbuf;
    register uint32_t* bPal = conv_color;
    register uint32_t* gPal = conv_color + 256;
    register uint32_t* rPal = conv_color + 512;
    // Render each character
    // Bit order matches render_text_line: bits 1,0 are leftmost pair, etc.
    while (row_data != row_data_end) {
        // Get glyph data for this scanline
        register uint8_t glyph = glyph_line[*row_data++ << 4];
        uint8_t attr = *row_data++;
        // Get foreground and background colors
        uint8_t fg_color0 = attr & 0b00001111;
        uint8_t bg_color1 = attr & 0b01110000;
        uint8_t bg_color0 = bg_color1 >> 4;
        uint8_t fg_color1 = fg_color0 << 4;
        register uint8_t pix01 = ((glyph & 0b00000001) ? fg_color1 : bg_color1) | ((glyph & 0b00000010) ? fg_color0 : bg_color0);
        register uint8_t pix23 = ((glyph & 0b00000100) ? fg_color1 : bg_color1) | ((glyph & 0b00001000) ? fg_color0 : bg_color0);
        register uint8_t pix45 = ((glyph & 0b00010000) ? fg_color1 : bg_color1) | ((glyph & 0b00100000) ? fg_color0 : bg_color0);
        register uint8_t pix67 = ((glyph & 0b01000000) ? fg_color1 : bg_color1) | ((glyph & 0b10000000) ? fg_color0 : bg_color0);

        *targetB++ = bPal[pix01];
        *targetB++ = bPal[pix23];
        *targetB++ = bPal[pix45];
        *targetB++ = bPal[pix67];

        *targetG++ = 0xbf203;// gPal[pix01];
        *targetG++ = 0xbf203; //gPal[pix23];
        *targetG++ = 0xbf203; //gPal[pix45];
        *targetG++ = 0xbf203; //gPal[pix67];

        *targetR++ = 0xbf203; //rPal[pix01];
        *targetR++ = 0xbf203; //rPal[pix23];
        *targetR++ = 0xbf203; //rPal[pix45];
        *targetR++ = 0xbf203; //rPal[pix67];
    }
}

static uint __time_critical_func(tmds_encoder)(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    bool is_xnor = false;
    if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0))) is_xnor = true;
    uint16_t d_out = d8 & 1;
    uint16_t qi = d_out;
    for (int i = 1; i < 8; i++) {
        d_out |= ((qi << 1) ^ (d8 & (1 << i))) ^ (is_xnor << i);
        qi = d_out & (1 << i);
    }
    if (is_xnor) d_out |= 1 << 9;
    else d_out |= 1 << 8;
    return d_out;
}

// инвертировать sym корректно по TMDS
inline static uint tmds_invert(uint sym) {
    return (sym ^ 0xFF)       // инвертировать биты 7:0
         | (sym & (1 << 8))   // сохранить бит 8
         | (1 << 9);          // установить бит 9 = "инвертировано"
}

inline static uint tmds2(uint8_t B1, uint8_t B2) {
    uint q_m1 = tmds_encoder(B1);  // только этап 1, без балансировки
    uint q_m2 = tmds_encoder(B2);
    uint s1  = q_m1;
    uint s1i = tmds_invert(q_m1);
    uint s2  = q_m2;
    uint s2i = tmds_invert(q_m2);

    uint sym12 = s1 | (s2 << 10);
    if (__builtin_popcount(sym12 & 0xFFFFF) != 10) {
        sym12 = s1i | (s2 << 10);
        if (__builtin_popcount(sym12 & 0xFFFFF) != 10) {
            sym12 = s1 | (s2i << 10);
            if (__builtin_popcount(sym12 & 0xFFFFF) != 10) {
                sym12 = s1i | (s2i << 10);
                if (__builtin_popcount(sym12 & 0xFFFFF) != 10) {
                    // нет решения — принять как есть или скорректировать цвет
                    sym12 = 0xbf203; // 2 white pixels to visualize it
                }
            }
        }
    }
    return sym12;
}

void __not_in_flash_func(graphics_set_palette_dvi2)(
    uint8_t R1, uint8_t G1, uint8_t B1, // left point
    uint8_t R2, uint8_t G2, uint8_t B2, // right point
    uint8_t i // 0-3 bits - left point color, 4-7 - right point color
) {
    uint32_t* bPal = conv_color;
    uint32_t* gPal = conv_color + 256;
    uint32_t* rPal = conv_color + 512;
    bPal[i] = 0xbf203; // tmds2(B1, B2);
    gPal[i] = 0xbf203; // tmds2(G1, G2);
    rPal[i] = 0xbf203; // tmds2(R1, R2);
}

void pre_render_line(void);

void __not_in_flash_func(core1_libdvi)() {
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    vga_initialized = true;

    audio_set_enabled(false);
    audio_init();
    audio_set_volume(config_get_volume());
    audio_set_enabled(true);
    config_clear_changes();

    // Заполнение палитры — CGA 16 цветов (индексы 0-15)
    // Формат cga_colors: 6-бит RRGGBB (как в VGA DAC)
    static uint8_t cga_colors[16][3] = {
        { 0,  0,  0},  //  0: Black
        { 0,  0, 42},  //  1: Blue        (0x02 -> b=2/3*63)
        { 0, 42,  0},  //  2: Green
        { 0, 42, 42},  //  3: Cyan
        {42,  0,  0},  //  4: Red
        {42,  0, 42},  //  5: Magenta
        {42, 21,  0},  //  6: Brown
        {42, 42, 42},  //  7: Light Gray
        {21, 21, 21},  //  8: Dark Gray
        {21, 21, 63},  //  9: Light Blue
        {21, 63, 21},  // 10: Light Green
        {21, 63, 63},  // 11: Light Cyan
        {63, 21, 21},  // 12: Light Red
        {63, 21, 63},  // 13: Light Magenta
        {63, 63, 21},  // 14: Yellow
        {63, 63, 63},  // 15: White
    };

    // заполнение палитры (text) 4 старших bit первый пиксел, 4 младших - второй
    for (int c1 = 0; c1 < 16; ++c1) {
        const uint8_t* c13 = cga_colors[c1];
        for (int c2 = 0; c2 < 16; ++c2) {
            const uint8_t* c23 = cga_colors[c2];
            int ci = c1 << 4 | c2; // compound index
            graphics_set_palette_dvi2(
                c13[0] << 2, c13[1] << 2, c13[2] << 2,
                c23[0] << 2, c23[1] << 2, c23[2] << 2,
                ci
            );
        }
    }


  //  while(!initialized) {
  //      sleep_ms(1);
  //      __dmb();
  //  }

    while (true) {
		for (uint y = 0; y < FRAME_HEIGHT; ++y) {
      //      repeat_me_often(); // TODO: ensure
            pre_render_line();
			uint32_t *tmdsbuf;
			queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
            if (osd_is_visible()) {
                osd_render_line_dvi(y, tmdsbuf);
            }
            else { // unknown mode
                for (int plane = 0; plane < 3; ++plane) {
                    uint32_t* target = tmdsbuf + plane * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD);
                    for (uint x8 = 0; x8 < FRAME_WIDTH / 8; ++x8) { // x8 = x pos *8
                        *target++ = 0x7f103; // pixel 0..1 black
                        *target++ = 0xbf203; // pixel 2..3 white
                        *target++ = 0x7f103; // pixel 4..5 black
                        *target++ = 0xbf203; // pixel 6..7 white
                    }
                }
            }
			queue_add_blocking(&dvi0.q_tmds_valid, &tmdsbuf);
		}
	}
}
#endif

//=============================================================================
// Global State
//=============================================================================

// Framebuffer for VGA output (in PSRAM)
static uint8_t *framebuffer = NULL;

// FatFS state
static FATFS fatfs;

//=============================================================================
// Error Display
//=============================================================================

/**
 * Display a fatal error screen (red box on black background).
 * Halts execution after displaying the message.
 * Can only display errors if VGA is initialized.
 */
static void show_error_screen(const char *title, const char *message, const char *detail) {
    if (!vga_initialized) {
        // VGA not ready, just print to serial and halt
        printf("FATAL ERROR: %s\n", title);
        printf("  %s\n", message);
        if (detail) printf("  %s\n", detail);
        while (1) { sleep_ms(1000); }
    }

    // Initialize OSD for error display
    osd_init();
    osd_clear();

    // Fill screen with black
    uint8_t black_attr = OSD_ATTR(OSD_WHITE, OSD_BLACK);
    osd_fill(0, 0, OSD_COLS, OSD_ROWS, ' ', black_attr);

    // Draw red error box in center
    int box_w = 60;
    int box_h = 10;
    int box_x = (OSD_COLS - box_w) / 2;
    int box_y = (OSD_ROWS - box_h) / 2;

    uint8_t error_attr = OSD_ATTR(OSD_WHITE, OSD_RED);
    uint8_t text_attr = OSD_ATTR(OSD_YELLOW, OSD_RED);

    // Fill box background
    osd_fill(box_x, box_y, box_w, box_h, ' ', error_attr);

    // Draw box border
    osd_draw_box_titled(box_x, box_y, box_w, box_h, title, error_attr);

    // Print message
    int msg_y = box_y + 3;
    osd_print(box_x + 3, msg_y, message, text_attr);

    // Print detail if provided
    if (detail && detail[0]) {
        osd_print(box_x + 3, msg_y + 2, detail, error_attr);
    }

    // Print hint at bottom
    osd_print(box_x + 3, box_y + box_h - 2, "Please check hardware and restart.", error_attr);

    osd_show();

    // Also print to serial
    printf("FATAL ERROR: %s\n", title);
    printf("  %s\n", message);
    if (detail) printf("  %s\n", detail);

    // Halt
    while (1) {
        sleep_ms(1000);
    }
}

/**
 * Display a warning screen (yellow box) but continue execution.
 */
static void show_warning_screen(const char *title, const char *message, int delay_ms) {
    if (!vga_initialized) {
        printf("WARNING: %s - %s\n", title, message);
        return;
    }

    osd_init();
    osd_clear();

    // Fill screen with black
    uint8_t black_attr = OSD_ATTR(OSD_WHITE, OSD_BLACK);
    osd_fill(0, 0, OSD_COLS, OSD_ROWS, ' ', black_attr);

    // Draw yellow warning box
    int box_w = 60;
    int box_h = 8;
    int box_x = (OSD_COLS - box_w) / 2;
    int box_y = (OSD_ROWS - box_h) / 2;

    uint8_t warn_attr = OSD_ATTR(OSD_BLACK, OSD_YELLOW);

    osd_fill(box_x, box_y, box_w, box_h, ' ', warn_attr);
    osd_draw_box_titled(box_x, box_y, box_w, box_h, title, warn_attr);
    osd_print(box_x + 3, box_y + 3, message, warn_attr);

    osd_show();

    printf("WARNING: %s - %s\n", title, message);
    sleep_ms(delay_ms);

    osd_hide();
}

//=============================================================================
// Platform HAL Implementation
//=============================================================================

/**
 * Get microsecond timestamp.
 */
uint32_t __not_in_flash_func(get_uticks)(void) {
    return time_us_32();
}

/**
 * Allocate memory (uses PSRAM for large allocations).
 */
void *pcmalloc(long size) {
    return malloc(size);
}

/**
 * Load ROM file from SD card to memory.
 */
int load_rom(void *phys_mem, const char *file, uint32_t addr, int backward) {
    FIL fp;
    FRESULT res;
    UINT bytes_read;

    char path[256];
    snprintf(path, sizeof(path), "pce/%s", file);

    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) {
        printf("Failed to open ROM: %s (error %d)\n", path, res);
        return -1;
    }

    FSIZE_t size = f_size(&fp);

    uint8_t *dest;
    if (backward) {
        // Load so ROM ends at addr (for BIOS - should end at 1MB boundary)
        dest = (uint8_t *)phys_mem + addr - size;
        DBG_PRINT("Loading ROM: %s (%lu bytes) at 0x%08lx-0x%08lx (dest=%p)\n",
               file, (unsigned long)size,
               (unsigned long)(addr - size), (unsigned long)(addr - 1), dest);
    } else {
        dest = (uint8_t *)phys_mem + addr;
        DBG_PRINT("Loading ROM: %s (%lu bytes) at 0x%08lx (dest=%p)\n",
               file, (unsigned long)size, (unsigned long)addr, dest);
    }

    res = f_read(&fp, dest, size, &bytes_read);
    if (res != FR_OK || bytes_read != size) {
        f_close(&fp);
        printf("ERROR: Failed to read ROM: %s (error %d, read %u of %lu)\n",
               file, res, bytes_read, (unsigned long)size);
        return -1;
    }

    f_close(&fp);

    // Debug: verify data was written to memory
    DBG_PRINT("  First bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           dest[0], dest[1], dest[2], dest[3],
           dest[4], dest[5], dest[6], dest[7]);
    DBG_PRINT("  Last bytes:  %02x %02x %02x %02x %02x %02x %02x %02x\n",
           dest[size-8], dest[size-7], dest[size-6], dest[size-5],
           dest[size-4], dest[size-3], dest[size-2], dest[size-1]);

    return (int)size;  // Return size on success
}

//=============================================================================
// VGA Redraw Callback - Bridge emulator VGA state to hardware driver
//=============================================================================

static void vga_redraw(void *opaque, int x, int y, int w, int h) {
    (void)opaque;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    // No action needed - VGA updates are handled in the main loop
}

//=============================================================================
// Keyboard Polling
//=============================================================================

// Track modifier key state for Win+F12 hotkey
static bool win_key_pressed = false;

// Process a single keycode, handling disk UI and settings UI hotkeys
// Returns true if key should be passed to emulator, false if consumed
static bool process_keycode(int is_down, int keycode) {
    // Track Win key state
    if (keycode == KEY_LEFTMETA) {
        win_key_pressed = is_down;
    }

    // Check for Win+F12 hotkey to toggle disk UI
    if (is_down && keycode == KEY_F12 && win_key_pressed) {
        if (!diskui_is_open() && !settingsui_is_open()) {
            // Open disk UI and pause emulation
            diskui_open();
        //    if (pc) {
        //        pc->paused = 1;
                audio_set_enabled(false);
        //    }
        } else if (diskui_is_open()) {
            // Close disk UI and resume emulation
            diskui_close();
        //    if (pc) {
        //        pc->paused = 0;
                audio_set_enabled(true);
        //    }
        }
        return false;  // Don't pass to emulator
    }

    // Check for Win+F11 hotkey to toggle settings UI
    if (is_down && keycode == KEY_F11 && win_key_pressed) {
        if (!settingsui_is_open() && !diskui_is_open()) {
            // Open settings UI and pause emulation
            settingsui_open();
        //    if (pc) {
        //        pc->paused = 1;
                audio_set_enabled(false);
        //    }
        } else if (settingsui_is_open()) {
            // Close settings UI and resume emulation
            settingsui_close();
        //    if (pc) {
        //        pc->paused = 0;
                audio_set_enabled(true);
        //    }
        }
        return false;  // Don't pass to emulator
    }

    // When disk UI is open, route all keys to it
    if (diskui_is_open()) {
        diskui_handle_key(keycode, is_down);

        // Check if disk UI was closed by Escape
        if (!diskui_is_open()) {// && pc && pc->paused) {
        //    pc->paused = 0;
            audio_set_enabled(true);
        }
        return false;  // Don't pass to emulator
    }

    // When settings UI is open, route all keys to it
    if (settingsui_is_open()) {
        settingsui_handle_key(keycode, is_down);

        // Check if settings UI was closed by Escape
        if (!settingsui_is_open()) { // && pc && pc->paused) {
        //    pc->paused = 0;
            audio_set_enabled(true);
        }
        return false;  // Don't pass to emulator
    }

    return true;  // Pass to emulator
}

static void poll_keyboard(void) {
    // Poll PS/2 keyboard
    ps2kbd_tick();

    int is_down, keycode;
    while (ps2kbd_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode)) {
        //    if (pc && pc->kbd) {
        //        ps2_put_keycode(pc->kbd, is_down, keycode);
        //    }
        }
    }

    // Poll PS/2 mouse (only if enabled and not paused)
    /*
    if (pc && pc->mouse_enabled && !pc->paused) {
        int16_t dx, dy;
        int8_t dz;
        uint8_t buttons;
        if (ps2mouse_get_state(&dx, &dy, &dz, &buttons)) {
            if (pc->mouse) {
                ps2_mouse_event(pc->mouse, dx, dy, dz, buttons);
            }
        }
    }
    */

#ifdef USB_HID_ENABLED
    // Poll USB keyboard
    usbkbd_tick();

    while (usbkbd_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode)) {
        //    if (pc && pc->kbd) {
        //        ps2_put_keycode(pc->kbd, is_down, keycode);
        //    }
        }
    }

    // Poll USB mouse (only if enabled and not paused)
    /*if (pc && pc->mouse_enabled && !pc->paused) {
        int16_t dx, dy;
        int8_t dz;
        uint8_t buttons;
        if (usbmouse_get_event(&dx, &dy, &dz, &buttons)) {
            if (pc->mouse) {
                ps2_mouse_event(pc->mouse, dx, dy, dz, buttons);
            }
        }
    }*/
#endif
}

//=============================================================================
// Platform Poll Callback
//=============================================================================

static void platform_poll(void *opaque) {
    (void)opaque;
    poll_keyboard();
    // VGA update is handled by Core 1, don't call here to avoid contention
}

//=============================================================================
// Configuration Loading
//=============================================================================

static void load_default_config(void) {
//
}

static int load_config_from_sd(const char *filename) {
    FIL fp;
    FRESULT res;
    DIR dir;
    FILINFO fno;

    // Debug: List 386 directory contents
    DBG_PRINT("Checking SD card contents...\n");
    res = f_opendir(&dir, "pce");
    if (res == FR_OK) {
        DBG_PRINT("  pce/ directory found, contents:\n");
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            DBG_PRINT("    %s%s (%lu bytes)\n",
                   fno.fname,
                   (fno.fattrib & AM_DIR) ? "/" : "",
                   (unsigned long)fno.fsize);
        }
        f_closedir(&dir);
    } else {
        DBG_PRINT("  pce/ directory not found (error %d)\n", res);
        // Try root directory
        res = f_opendir(&dir, "");
        if (res == FR_OK) {
            DBG_PRINT("  Root directory contents:\n");
            while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
                DBG_PRINT("    %s%s\n", fno.fname, (fno.fattrib & AM_DIR) ? "/" : "");
            }
            f_closedir(&dir);
        }
    }

    char path[256];
    snprintf(path, sizeof(path), "pce/%s", filename);

    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) {
        DBG_PRINT("Config file not found: %s (error %d)\n", path, res);
        return -1;
    }

    DBG_PRINT("Loading config: %s\n", path);

    // Read entire file
    FSIZE_t size = f_size(&fp);
    char *content = malloc(size + 1);
    if (!content) {
        f_close(&fp);
        return -1;
    }

    UINT bytes_read;
    res = f_read(&fp, content, size, &bytes_read);
    f_close(&fp);

    if (res != FR_OK) {
        free(content);
        return -1;
    }

    content[size] = '\0';

    // Parse INI content
 //   if (ini_parse_string(content, parse_conf_ini, &config) != 0) {
 //       printf("Failed to parse config\n");
 //       free(content);
 //       return -1;
 //   }

    // Also parse pce-specific settings
    ini_parse_string(content, parse_pce_ini, NULL);

    free(content);
    return 0;
}

//=============================================================================
// Clock Configuration
//=============================================================================

// Flash timing configuration for overclocking
void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int cfg_flash) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = cfg_flash * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) {
        divisor = 2;
    }

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
        rxdelay += 1;
    }

    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

static void configure_clocks(void) {
#if CPU_CLOCK_MHZ > 252
    // Overclock: disable voltage limit and set higher voltage
    DBG_PRINT("Configuring overclock: %d MHz @ %s\n", CPU_CLOCK_MHZ,
           CPU_CLOCK_MHZ >= 504 ? "1.65V" :
           CPU_CLOCK_MHZ >= 378 ? "1.60V" : "1.50V");

    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    sleep_ms(100);  // Stabilization delay

    // Configure flash timing BEFORE changing clock
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_MAX_FREQ_MHZ);
#endif

    // Set system clock
    set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false);

    DBG_PRINT("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

/**
 * Get voltage for CPU frequency
 */
static enum vreg_voltage get_voltage_for_freq(int mhz) {
    int v = config_get_voltage();
    if (v >= 0) return (enum vreg_voltage)v;  /* user override */
    /* auto: safe defaults per frequency */
    if (mhz >= 504) return VREG_VOLTAGE_1_65;
    if (mhz >= 378) return VREG_VOLTAGE_1_60;
    return VREG_VOLTAGE_1_50;
}

/**
 * Reconfigure clocks at runtime based on INI settings.
 * This function MUST run from RAM, not flash.
 */
static void __no_inline_not_in_flash_func(reconfigure_clocks)(int cpu_mhz, int psram_mhz, uint psram_pin, int cfg_flash) {
    int current_mhz = clock_get_hz(clk_sys) / 1000000;

    DBG_PRINT("Reconfiguring clocks: %d MHz -> %d MHz, PSRAM: %d MHz, FLASH: %d\n",
              current_mhz, cpu_mhz, psram_mhz, cfg_flash);

    // Only change system clock if CPU frequency actually differs.
    // Unnecessary PLL reconfiguration disrupts PIO timing (HDMI, audio).
    if (cpu_mhz != current_mhz) {
        bool lowering = (cpu_mhz < current_mhz);
        enum vreg_voltage new_voltage = get_voltage_for_freq(cpu_mhz);

        if (lowering) {
            // LOWERING: clock first, then voltage (safe order)
            set_flash_timings(cpu_mhz, cfg_flash);
            set_sys_clock_khz(cpu_mhz * 1000, false);
            sleep_ms(10);
            vreg_set_voltage(new_voltage);
        } else {
            // RAISING: voltage first, then clock (safe order)
            vreg_disable_voltage_limit();
            vreg_set_voltage(new_voltage);
            sleep_ms(50);  // Stabilization delay
            set_flash_timings(cpu_mhz, cfg_flash);
            set_sys_clock_khz(cpu_mhz * 1000, false);
        }
    }

    // Re-initialize PSRAM with the new frequency
    psram_init_with_freq(psram_pin, psram_mhz);

    // Recalculate VGA PIO clock divider (vga_hw_init ran before this call)
    vga_hw_reclock();

    DBG_PRINT("Clock reconfiguration complete: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

//=============================================================================
// Hardware Initialization
//=============================================================================
static void core1_entry(void);
static bool init_hardware(void) {
    // Configure clocks (including overclock if enabled)
    configure_clocks();

    // Initialize PSRAM first
    DBG_PRINT("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    DBG_PRINT("  PSRAM CS pin: GPIO%d\n", psram_pin);
    psram_init(psram_pin);

    if (!psram_test()) {
        printf("ERROR: PSRAM test failed!\n");
        // Can't show visual error - VGA not ready yet
        return false;
    }
    DBG_PRINT("  PSRAM test passed (8MB)\n");

#if DVI_A
	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	// HDMI Audio related
	dvi_get_blank_settings(&dvi0)->top    = 4 * 0;
	dvi_get_blank_settings(&dvi0)->bottom = 4 * 0;
	dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
	dvi_set_audio_freq(&dvi0, 44100, 28000, 6272);
	add_repeating_timer_ms(-2, audio_timer_callback, NULL, &audio_timer);

	multicore_launch_core1(core1_libdvi);

#else
    // Initialize VGA early so we can show errors on screen
    multicore_launch_core1(core1_entry);
#endif

    while(!vga_initialized) {
        sleep_ms(1);
        __dmb();
    }
    __dmb();

    // Initialize SD card
    DBG_PRINT("Initializing SD card...\n");
    FRESULT res = f_mount(&fatfs, "", 1);
    if (res != FR_OK) {
        char detail[32];
        snprintf(detail, sizeof(detail), "FatFS error code: %d", res);
        show_error_screen(" SD Card Error ", "Failed to mount SD card.", detail);
        // show_error_screen never returns
    }
    DBG_PRINT("  SD card mounted\n");

    // Check if pce/ directory exists
    DIR dir;
    res = f_opendir(&dir, "386");
    if (res != FR_OK) {
        show_error_screen(" Missing Directory ", "Directory 'pce/' not found on SD card.", "Create it and add config.ini, bios.bin");
        // show_error_screen never returns
    }
    f_closedir(&dir);
    DBG_PRINT("  pce/ directory found\n");

    // Load murm386-specific hardware settings from INI
    // This allows cpu_freq and psram_freq to be configured
    {
        FIL fp;
        char *content = NULL;

        if (f_open(&fp, "pce/config.ini", FA_READ) == FR_OK) {
            FSIZE_t size = f_size(&fp);
            content = malloc(size + 1);
            if (content) {
                UINT bytes_read;
                if (f_read(&fp, content, size, &bytes_read) == FR_OK) {
                    content[bytes_read] = '\0';
                    // Parse just the [pce] section
                    ini_parse_string(content, parse_pce_ini, NULL);
                }
                free(content);
            }
            f_close(&fp);
            DBG_PRINT("  Loaded config.ini\n");
        } else {
            show_warning_screen(" Warning ", "config.ini not found, using defaults.", 2000);
        }

        // Check if clock reconfiguration is needed
        int cfg_cpu = config_get_cpu_freq();
        int cfg_psram = config_get_psram_freq();
        int cfg_flash = config_get_flash_freq();
        // If HDMI boosted the clock (to 504 MHz for jitter-free TMDS),
        // keep it — only reconfigure PSRAM frequency.
        extern bool SELECT_VGA;
        int cur_mhz = clock_get_hz(clk_sys) / 1000000;
        if (!SELECT_VGA && cur_mhz > cfg_cpu) {
            cfg_cpu = cur_mhz;  // preserve HDMI-boosted clock
        }
        if (cfg_cpu != CPU_CLOCK_MHZ || cfg_psram != PSRAM_MAX_FREQ_MHZ || cfg_flash != FLASH_MAX_FREQ_MHZ) {
            reconfigure_clocks(cfg_cpu, cfg_psram, psram_pin, cfg_flash);
        }
    }

    // Initialize PS/2 keyboard
    DBG_PRINT("Initializing PS/2 keyboard...\n");
    DBG_PRINT("  CLK: GPIO%d, DATA: GPIO%d\n", PS2_PIN_CLK, PS2_PIN_DATA);
    ps2kbd_init(PS2_PIN_CLK);

    // Initialize PS/2 mouse
    DBG_PRINT("Initializing PS/2 mouse...\n");
    DBG_PRINT("  CLK: GPIO%d, DATA: GPIO%d\n", PS2_MOUSE_CLK, PS2_MOUSE_DATA);
    ps2mouse_init();

    // Initialize USB HID keyboard (if enabled)
#ifdef USB_HID_ENABLED
    DBG_PRINT("Initializing USB HID keyboard...\n");
    usbkbd_init();
#endif
    return true;
}

//=============================================================================
// Emulator Initialization
//=============================================================================

static bool init_emulator(void) {
    // Load configuration
    load_default_config();

    // Try to load config from SD card
    if (load_config_from_sd("config.ini") != 0) {
        DBG_PRINT("Using default configuration\n");
    }

    DBG_PRINT("\nEmulator configuration:\n");
    DBG_PRINT("  Memory: %ld MB\n", config.mem_size / (1024 * 1024));
    DBG_PRINT("  VGA Memory: %ld KB\n", config.vga_mem_size / 1024);
    DBG_PRINT("  CPU: %d86\n", config.cpu_gen);
    DBG_PRINT("  BIOS: %s\n", config.bios ? config.bios : "(none)");
    DBG_PRINT("  VGA BIOS: %s\n", config.vga_bios ? config.vga_bios : "(none)");
    DBG_PRINT("  Floppy A: %s\n", config.fdd[0] ? config.fdd[0] : "(none)");
    DBG_PRINT("  Floppy B: %s\n", config.fdd[1] ? config.fdd[1] : "(none)");

    // Initialize disk UI
    DBG_PRINT("Initializing Disk UI...\n");
    diskui_init();

    // Initialize settings UI
    DBG_PRINT("Initializing Settings UI...\n");
    settingsui_init();

    // Hardware settings are loaded from [pce] section via parse_pce_ini
    config_clear_changes();

    return true;
}

static void __not_in_flash_func(core1_entry)(void) {

    DBG_PRINT("[Core 1] Initializing VGA...\n");
    DBG_PRINT("  Base pin: GPIO%d\n", VGA_BASE_PIN);
    vga_hw_init();
    sleep_ms(100);
    vga_initialized = true;

    // Initialize I2S Audio
    DBG_PRINT("Initializing I2S Audio...\n");
    DBG_PRINT("  DATA: GPIO%d, CLK: GPIO%d, LRCK: GPIO%d\n",
           I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, I2S_CLOCK_PIN_BASE + 1);
    audio_set_enabled(false);
    audio_init();
    audio_set_volume(config_get_volume());
    audio_set_enabled(true);
    config_clear_changes();
    while(!initialized) {
        sleep_ms(1);
        __dmb();
    }
    static repeating_timer_t m_timer = { 0 };
    int hz = 44100;
	add_repeating_timer_us(-1000000 / hz, timer_callback0, NULL, &m_timer);
    while(1) {
        repeat_me_often();
        sleep_us(1);
    }
    __unreachable();
}

//=============================================================================
// Welcome Screen
//=============================================================================

static void show_welcome_screen(void) {
    // Welcome screen dimensions
    int wx = 14, wy = 7, ww = 51, wh = 11;

    osd_clear();

    // Draw the window content once (static, won't flicker)
    osd_draw_box(wx, wy, ww, wh, OSD_ATTR_BORDER);
    osd_fill(wx + 1, wy + 1, ww - 2, wh - 2, ' ', OSD_ATTR_NORMAL);

    // Title
    osd_print_center(wy + 2, "PCE Macintosh Plus", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Version
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "Version %d.%d.%d", PCE_VERSION_MAJOR, PCE_VERSION_MINOR, PCE_VERSION_ITERATION);
    osd_print_center(wy + 4, version_str, OSD_ATTR_NORMAL);

    // Author
    osd_print_center(wy + 5, "Hampa Hug <hampa@hampa.ch>. Port by Mike V73", OSD_ATTR_NORMAL);

    // Hardware info
    char hw_str[50];
    snprintf(hw_str, sizeof(hw_str), "RP2350 @ %d MHz / PSRAM @ %d MHz / FLASH @ %d MHz",
             config_get_cpu_freq(), config_get_psram_freq(), config_get_flash_freq());
    osd_print_center(wy + 7, hw_str, OSD_ATTR(OSD_LIGHTCYAN, OSD_BLUE));

    // Platform (green text)
#ifdef BOARD_M1
    osd_print_center(wy + 8, "Platform: M1", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_M2)
    osd_print_center(wy + 8, "Platform: M2", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_PC)
    osd_print_center(wy + 8, "Platform: Olimex PICO-PC", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_Z2)
    osd_print_center(wy + 8, "Platform: RP2350-PiZero", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#else
    osd_print_center(wy + 8, "Platform: Unknown", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#endif

    osd_show();

    // Animate plasma background for 7 seconds (700 frames at ~10ms each)
    // Window area is skipped by osd_draw_plasma_background, so it won't flicker
    for (int frame = 0; frame < 700; frame++) {
        ps2kbd_tick();
        int is_down = 0, keycode = 0;
        ps2kbd_get_key(&is_down, &keycode);
        if (is_down) break;
        osd_draw_plasma_background(frame * 3, wx, wy, ww, wh);
        sleep_ms(10);
    }

    osd_hide();
}

//=============================================================================
// Main Entry Point
//=============================================================================

int main(void) {
    // Initialize stdio (USB Serial or UART depending on USB HID mode)
    stdio_init_all();
    #ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    #endif
    DBG_PRINT("\n\n");
    DBG_PRINT("============================================\n");
    DBG_PRINT("  PCE - Macintosh Plus Emulator for RP2350\n");
    DBG_PRINT("  Version %d.%d.%d\n", PCE_VERSION_MAJOR, PCE_VERSION_MINOR, PCE_VERSION_ITERATION);
    DBG_PRINT("============================================\n\n");

#ifndef USB_HID_ENABLED
    // Wait for USB Serial connection (with timeout)
    // Only when USB CDC is enabled (USB HID disabled)
    DBG_PRINT("Waiting for USB Serial connection...\n");
    DBG_PRINT("(Press any key or wait %d seconds)\n\n", USB_CONSOLE_DELAY_MS / 1000);

    absolute_time_t deadline = make_timeout_time_ms(USB_CONSOLE_DELAY_MS);
    while (!stdio_usb_connected() && !time_reached(deadline)) {
        sleep_ms(100);
    }

    if (stdio_usb_connected()) {
        DBG_PRINT("USB Serial connected!\n\n");
    } else {
        DBG_PRINT("Timeout - continuing without USB Serial\n\n");
    }
#else
    // USB HID mode: using UART for debug output
    DBG_PRINT("USB HID mode: USB port used for keyboard input\n");
    DBG_PRINT("Debug output via UART\n\n");
#endif

    // Print board configuration
    DBG_PRINT("Board Configuration:\n");
#ifdef BOARD_M1
    DBG_PRINT("  Board: M1\n");
#elif defined(BOARD_M2)
    DBG_PRINT("  Board: M2\n");
#else
    DBG_PRINT("  Board: Unknown\n");
#endif
    DBG_PRINT("  CPU Speed: %d MHz\n", CPU_CLOCK_MHZ);
    DBG_PRINT("  PSRAM Speed: %d MHz\n", PSRAM_MAX_FREQ_MHZ);
    DBG_PRINT("\n");

    // Initialize hardware
    if (!init_hardware()) {
        printf("\nHardware initialization failed!\n");
        while (true) {
            sleep_ms(1000);
        }
    }

    // Initialize emulator
    if (!init_emulator()) {
        printf("\nEmulator initialization failed!\n");
        while (true) {
            sleep_ms(1000);
        }
    }

    initialized = true;

    // Show welcome screen
    if(*(uint32_t*)(0x20000000 + (512ul << 10) - 32) != 0x1927fa52) // magic to fast reboot
        show_welcome_screen();

    DBG_PRINT("\nStarting emulation...\n");

#if THROTTLING
    // Frame rate throttling for audio sync
    // Target ~60fps to match audio processing rate (16666us per frame)
    uint64_t frame_start_time = time_us_64();
    const uint64_t target_frame_time_us = 16666; // 60Hz = 16.666ms per frame
    int frame_step_count = 0;
    const int steps_per_frame = 100; // Number of outer loop iterations per frame
#endif
    // Retrace-based frame submission state
    static int last_vga_mode = -1;

    // Main emulation loop (Core 0)
    while (true) {
        // Skip CPU execution when paused (disk UI or settings UI active)
    //    if (pc->paused) {
            // Still poll keyboard to handle UI input
            poll_keyboard();

            // Animate plasma background for active UI
            if (diskui_is_open()) {
                diskui_animate();
            } else if (settingsui_is_open()) {
                settingsui_animate();
            }

            sleep_ms(16);  // ~60Hz polling/animation rate
            continue;
    //    }

        // Run CPU steps - batch multiple steps for efficiency
    //    for (int i = 0; i < 10; i++) {
    //        pc_step(pc);
    //    }

        // Poll keyboard less frequently (every 20 iterations ~5ms)
        // Keyboard events are buffered, so missing a few cycles is fine
        static int poll_counter = 0;
        if (++poll_counter >= 20) {
            poll_counter = 0;
            poll_keyboard();
        }

        // Check for settings UI restart request (requires full RP reset)
        if (settingsui_restart_requested()) {
            settingsui_clear_restart();
            DBG_PRINT("Settings changed - triggering RP reset...\n");
            // Full hardware reset via watchdog
            break;
        }
    }

    DBG_PRINT("\nEmulation stopped.\n");
    *(uint32_t*)(0x20000000 + (512ul << 10) - 32) = 0x1927fa52; // magic to fast reboot
    watchdog_reboot(0, 0, 0);
    while (true);
    __unreachable();
    return 0;
}
