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
#include "rp2350_mac.h"

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
			sample.channels[0] = 0;
			sample.channels[1] = 0;
			*(audio_ptr++) = sample;
			sample_count = sample_count + 1;
		}
		increase_write_pointer(&dvi0.audio_ring, size);
	}
}

#define FRAME_WIDTH 640
#define FRAME_HEIGHT (480 / DVI_VERTICAL_REPEAT)

#include "font8x16.h"
extern uint8_t text_buffer_sram[80 * 25 * 2];
static void __not_in_flash_func(osd_render_line_dvi)(uint line, uint32_t *tmdsbuf) {
    uint32_t char_row = line >> 4;
    const uint8_t* glyph_line = font_8x16 + (line & 15);
    if (char_row >= OSD_ROWS) return;
    uint8_t *row_data = &text_buffer_sram[char_row * OSD_COLS * 2];
    uint8_t *row_data_end = row_data + OSD_COLS * 2;
    uint32_t* targetR = tmdsbuf + 2 * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD);
    uint32_t* targetG = tmdsbuf + 1 * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD);
    uint32_t* targetB = tmdsbuf;
    register uint32_t* bPal = conv_color;
    register uint32_t* gPal = conv_color + 256;
    register uint32_t* rPal = conv_color + 512;
    while (row_data != row_data_end) {
        register uint8_t glyph = glyph_line[*row_data++ << 4];
        uint8_t attr = *row_data++;
        uint8_t fg_color0 = attr & 0b00001111;
        uint8_t bg_color1 = attr & 0b01110000;
        uint8_t bg_color0 = bg_color1 >> 4;
        uint8_t fg_color1 = fg_color0 << 4;
        register uint8_t pix01 = ((glyph & 0b00000001) ? fg_color1 : bg_color1) | ((glyph & 0b00000010) ? fg_color0 : bg_color0);
        register uint8_t pix23 = ((glyph & 0b00000100) ? fg_color1 : bg_color1) | ((glyph & 0b00001000) ? fg_color0 : bg_color0);
        register uint8_t pix45 = ((glyph & 0b00010000) ? fg_color1 : bg_color1) | ((glyph & 0b00100000) ? fg_color0 : bg_color0);
        register uint8_t pix67 = ((glyph & 0b01000000) ? fg_color1 : bg_color1) | ((glyph & 0b10000000) ? fg_color0 : bg_color0);
        *targetB++ = bPal[pix01]; *targetB++ = bPal[pix23]; *targetB++ = bPal[pix45]; *targetB++ = bPal[pix67];
        *targetG++ = 0xbf203; *targetG++ = 0xbf203; *targetG++ = 0xbf203; *targetG++ = 0xbf203;
        *targetR++ = 0xbf203; *targetR++ = 0xbf203; *targetR++ = 0xbf203; *targetR++ = 0xbf203;
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

inline static uint tmds_invert(uint sym) {
    return (sym ^ 0xFF) | (sym & (1 << 8)) | (1 << 9);
}

inline static uint tmds2(uint8_t B1, uint8_t B2) {
    uint q_m1 = tmds_encoder(B1);
    uint q_m2 = tmds_encoder(B2);
    uint s1  = q_m1; uint s1i = tmds_invert(q_m1);
    uint s2  = q_m2; uint s2i = tmds_invert(q_m2);
    uint sym12 = s1 | (s2 << 10);
    if (__builtin_popcount(sym12 & 0xFFFFF) != 10) {
        sym12 = s1i | (s2 << 10);
        if (__builtin_popcount(sym12 & 0xFFFFF) != 10) {
            sym12 = s1 | (s2i << 10);
            if (__builtin_popcount(sym12 & 0xFFFFF) != 10) {
                sym12 = s1i | (s2i << 10);
                if (__builtin_popcount(sym12 & 0xFFFFF) != 10) sym12 = 0xbf203;
            }
        }
    }
    return sym12;
}

void __not_in_flash_func(graphics_set_palette_dvi2)(
    uint8_t R1, uint8_t G1, uint8_t B1,
    uint8_t R2, uint8_t G2, uint8_t B2,
    uint8_t i
) {
    uint32_t* bPal = conv_color;
    uint32_t* gPal = conv_color + 256;
    uint32_t* rPal = conv_color + 512;
    bPal[i] = 0xbf203;
    gPal[i] = 0xbf203;
    rPal[i] = 0xbf203;
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
    static uint8_t cga_colors[16][3] = {
        { 0,  0,  0}, { 0,  0, 42}, { 0, 42,  0}, { 0, 42, 42},
        {42,  0,  0}, {42,  0, 42}, {42, 21,  0}, {42, 42, 42},
        {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
        {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
    };
    for (int c1 = 0; c1 < 16; ++c1) {
        const uint8_t* c13 = cga_colors[c1];
        for (int c2 = 0; c2 < 16; ++c2) {
            const uint8_t* c23 = cga_colors[c2];
            int ci = c1 << 4 | c2;
            graphics_set_palette_dvi2(
                c13[0] << 2, c13[1] << 2, c13[2] << 2,
                c23[0] << 2, c23[1] << 2, c23[2] << 2, ci);
        }
    }
    while (true) {
		for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            pre_render_line();
			uint32_t *tmdsbuf;
			queue_remove_blocking(&dvi0.q_tmds_free, &tmdsbuf);
            if (osd_is_visible()) {
                osd_render_line_dvi(y, tmdsbuf);
            } else {
                for (int plane = 0; plane < 3; ++plane) {
                    uint32_t* target = tmdsbuf + plane * (FRAME_WIDTH / DVI_SYMBOLS_PER_WORD);
                    for (uint x8 = 0; x8 < FRAME_WIDTH / 8; ++x8) {
                        *target++ = 0x7f103; *target++ = 0xbf203;
                        *target++ = 0x7f103; *target++ = 0xbf203;
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

static uint8_t *framebuffer = NULL;
static FATFS fatfs;

//=============================================================================
// Error Display
//=============================================================================

static void show_error_screen(const char *title, const char *message, const char *detail) {
    if (!vga_initialized) {
        printf("FATAL ERROR: %s\n", title);
        printf("  %s\n", message);
        if (detail) printf("  %s\n", detail);
        while (1) { sleep_ms(1000); }
    }
    osd_init(); osd_clear();
    uint8_t black_attr = OSD_ATTR(OSD_WHITE, OSD_BLACK);
    osd_fill(0, 0, OSD_COLS, OSD_ROWS, ' ', black_attr);
    int box_w = 60, box_h = 10;
    int box_x = (OSD_COLS - box_w) / 2;
    int box_y = (OSD_ROWS - box_h) / 2;
    uint8_t error_attr = OSD_ATTR(OSD_WHITE, OSD_RED);
    uint8_t text_attr = OSD_ATTR(OSD_YELLOW, OSD_RED);
    osd_fill(box_x, box_y, box_w, box_h, ' ', error_attr);
    osd_draw_box_titled(box_x, box_y, box_w, box_h, title, error_attr);
    int msg_y = box_y + 3;
    osd_print(box_x + 3, msg_y, message, text_attr);
    if (detail && detail[0]) osd_print(box_x + 3, msg_y + 2, detail, error_attr);
    osd_print(box_x + 3, box_y + box_h - 2, "Please check hardware and restart.", error_attr);
    osd_show();
    printf("FATAL ERROR: %s\n", title);
    printf("  %s\n", message);
    if (detail) printf("  %s\n", detail);
    while (1) { sleep_ms(1000); }
}

static void show_warning_screen(const char *title, const char *message, int delay_ms) {
    if (!vga_initialized) { printf("WARNING: %s - %s\n", title, message); return; }
    osd_init(); osd_clear();
    uint8_t black_attr = OSD_ATTR(OSD_WHITE, OSD_BLACK);
    osd_fill(0, 0, OSD_COLS, OSD_ROWS, ' ', black_attr);
    int box_w = 60, box_h = 8;
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

uint32_t __not_in_flash_func(get_uticks)(void) { return time_us_32(); }

void *pcmalloc(long size) { return malloc(size); }

int load_rom(void *phys_mem, const char *file, uint32_t addr, int backward) {
    FIL fp; FRESULT res; UINT bytes_read;
    char path[256];
    snprintf(path, sizeof(path), "pce/%s", file);
    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) { printf("Failed to open ROM: %s (error %d)\n", path, res); return -1; }
    FSIZE_t size = f_size(&fp);
    uint8_t *dest;
    if (backward) {
        dest = (uint8_t *)phys_mem + addr - size;
    } else {
        dest = (uint8_t *)phys_mem + addr;
    }
    res = f_read(&fp, dest, size, &bytes_read);
    if (res != FR_OK || bytes_read != size) {
        f_close(&fp);
        printf("ERROR: Failed to read ROM: %s\n", file);
        return -1;
    }
    f_close(&fp);
    return (int)size;
}

static void vga_redraw(void *opaque, int x, int y, int w, int h) {
    (void)opaque; (void)x; (void)y; (void)w; (void)h;
}

//=============================================================================
// Keyboard Polling
//=============================================================================

static bool win_key_pressed = false;

static bool process_keycode(int is_down, int keycode) {
    if (keycode == KEY_LEFTMETA) win_key_pressed = is_down;

    if (is_down && keycode == KEY_F12 && win_key_pressed) {
        if (!diskui_is_open() && !settingsui_is_open()) {
            diskui_open();
            audio_set_enabled(false);
        } else if (diskui_is_open()) {
            diskui_close();
            audio_set_enabled(true);
        }
        return false;
    }
    if (is_down && keycode == KEY_F11 && win_key_pressed) {
        if (!settingsui_is_open() && !diskui_is_open()) {
            settingsui_open();
            audio_set_enabled(false);
        } else if (settingsui_is_open()) {
            settingsui_close();
            audio_set_enabled(true);
        }
        return false;
    }
    if (diskui_is_open()) {
        diskui_handle_key(keycode, is_down);
        if (!diskui_is_open()) audio_set_enabled(true);
        return false;
    }
    if (settingsui_is_open()) {
        settingsui_handle_key(keycode, is_down);
        if (!settingsui_is_open()) audio_set_enabled(true);
        return false;
    }
    return true;
}

static void poll_keyboard(void) {
    ps2kbd_tick();
    int is_down, keycode;
    while (ps2kbd_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode))
            rp2350_mac_send_key(is_down, keycode);
    }

    // PS/2 mouse
    {
        int16_t dx, dy; int8_t dz; uint8_t buttons;
        if (ps2mouse_get_state(&dx, &dy, &dz, &buttons))
            rp2350_mac_send_mouse(dx, -dy, buttons);
    }

#ifdef USB_HID_ENABLED
    usbkbd_tick();
    while (usbkbd_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode))
            rp2350_mac_send_key(is_down, keycode);
    }
#endif
}

//=============================================================================
// Clock Configuration
//=============================================================================

void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int cfg_flash) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = cfg_flash * 1000000;
    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;
    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

static void configure_clocks(void) {
#if CPU_CLOCK_MHZ > 252
    DBG_PRINT("Configuring overclock: %d MHz @ %s\n", CPU_CLOCK_MHZ,
           CPU_CLOCK_MHZ >= 504 ? "1.65V" :
           CPU_CLOCK_MHZ >= 378 ? "1.60V" : "1.50V");
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    sleep_ms(100);
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_MAX_FREQ_MHZ);
#endif
    set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false);
    DBG_PRINT("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

static enum vreg_voltage get_voltage_for_freq(int mhz) {
    int v = config_get_voltage();
    if (v >= 0) return (enum vreg_voltage)v;
    if (mhz >= 504) return VREG_VOLTAGE_1_65;
    if (mhz >= 378) return VREG_VOLTAGE_1_60;
    return VREG_VOLTAGE_1_50;
}

static void __no_inline_not_in_flash_func(reconfigure_clocks)(int cpu_mhz, int psram_mhz, uint psram_pin, int cfg_flash) {
    int current_mhz = clock_get_hz(clk_sys) / 1000000;
    if (cpu_mhz != current_mhz) {
        bool lowering = (cpu_mhz < current_mhz);
        enum vreg_voltage new_voltage = get_voltage_for_freq(cpu_mhz);
        if (lowering) {
            set_flash_timings(cpu_mhz, cfg_flash);
            set_sys_clock_khz(cpu_mhz * 1000, false);
            sleep_ms(10);
            vreg_set_voltage(new_voltage);
        } else {
            vreg_disable_voltage_limit();
            vreg_set_voltage(new_voltage);
            sleep_ms(50);
            set_flash_timings(cpu_mhz, cfg_flash);
            set_sys_clock_khz(cpu_mhz * 1000, false);
        }
    }
    psram_init_with_freq(psram_pin, psram_mhz);
    vga_hw_reclock();
    DBG_PRINT("Clock reconfiguration complete: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

//=============================================================================
// Hardware Initialization
//=============================================================================
static void core1_entry(void);
static bool init_hardware(void) {
    configure_clocks();

    DBG_PRINT("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    psram_init(psram_pin);
    if (!psram_test()) { printf("ERROR: PSRAM test failed!\n"); return false; }
    DBG_PRINT("  PSRAM test passed\n");

#if DVI_A
	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	dvi_get_blank_settings(&dvi0)->top    = 4 * 0;
	dvi_get_blank_settings(&dvi0)->bottom = 4 * 0;
	dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
	dvi_set_audio_freq(&dvi0, 44100, 28000, 6272);
	add_repeating_timer_ms(-2, audio_timer_callback, NULL, &audio_timer);
	multicore_launch_core1(core1_libdvi);
#else
    multicore_launch_core1(core1_entry);
#endif

    while(!vga_initialized) { sleep_ms(1); __dmb(); }
    __dmb();

    DBG_PRINT("Initializing SD card...\n");
    FRESULT res = f_mount(&fatfs, "", 1);
    if (res != FR_OK) {
        char detail[32];
        snprintf(detail, sizeof(detail), "FatFS error code: %d", res);
        show_error_screen(" SD Card Error ", "Failed to mount SD card.", detail);
    }
    DBG_PRINT("  SD card mounted\n");

    DIR dir;
    res = f_opendir(&dir, "pce");   /* was "386" — fixed */
    if (res != FR_OK) {
        show_error_screen(" Missing Directory ",
                          "Directory 'pce/' not found on SD card.",
                          "Create it and add config.ini, mac-128k.rom, system.img");
    }
    f_closedir(&dir);
    DBG_PRINT("  pce/ directory found\n");

    // Load [pce] hardware settings for clock reconfigure
    {
        FIL fp;
        if (f_open(&fp, "pce/hardware.ini", FA_READ) == FR_OK) {
            FSIZE_t size = f_size(&fp);
            char *content = malloc(size + 1);
            if (content) {
                UINT bytes_read;
                if (f_read(&fp, content, size, &bytes_read) == FR_OK) {
                    content[bytes_read] = '\0';
                    ini_parse_string(content, parse_pce_ini, NULL);
                }
                free(content);
            }
            f_close(&fp);
            DBG_PRINT("  Loaded hardware.ini\n");
        } else {
            show_warning_screen(" Warning ", "hardware.ini not found, using defaults.", 2000);
        }

        int cfg_cpu = config_get_cpu_freq();
        int cfg_psram = config_get_psram_freq();
        int cfg_flash = config_get_flash_freq();
        extern bool SELECT_VGA;
        int cur_mhz = clock_get_hz(clk_sys) / 1000000;
        if (!SELECT_VGA && cur_mhz > cfg_cpu) cfg_cpu = cur_mhz;
        if (cfg_cpu != CPU_CLOCK_MHZ || cfg_psram != PSRAM_MAX_FREQ_MHZ || cfg_flash != FLASH_MAX_FREQ_MHZ) {
            reconfigure_clocks(cfg_cpu, cfg_psram, psram_pin, cfg_flash);
        }
    }

    DBG_PRINT("Initializing PS/2 keyboard...\n");
    ps2kbd_init(PS2_PIN_CLK);

    DBG_PRINT("Initializing PS/2 mouse...\n");
    ps2mouse_init();

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
    DBG_PRINT("Initializing Disk UI...\n");
    diskui_init();

    DBG_PRINT("Initializing Settings UI...\n");
    settingsui_init();

    DBG_PRINT("Initializing Mac Plus emulator...\n");
    const char* err = rp2350_mac_init();
    if (err) {
        show_error_screen(" Emulator Error ",
                          err,
                          "Check pce/config.ini, pce/mac-128k.rom, pce/system.img");
        /* show_error_screen never returns */
    }

    config_clear_changes();
    return true;
}

static void __not_in_flash_func(core1_entry)(void) {
    DBG_PRINT("[Core 1] Initializing VGA...\n");
    vga_hw_init();
    sleep_ms(100);
    vga_initialized = true;

    DBG_PRINT("Initializing I2S Audio...\n");
    audio_set_enabled(false);
    audio_init();
    audio_set_volume(config_get_volume());
    audio_set_enabled(true);
    config_clear_changes();
    while(!initialized) { sleep_ms(1); __dmb(); }
    static repeating_timer_t m_timer = { 0 };
    int hz = 44100;
	add_repeating_timer_us(-1000000 / hz, timer_callback0, NULL, &m_timer);
    while(1) { repeat_me_often(); sleep_us(1); }
    __unreachable();
}

//=============================================================================
// Welcome Screen
//=============================================================================

static void show_welcome_screen(void) {
    int wx = 14, wy = 7, ww = 51, wh = 11;
    osd_clear();
    osd_draw_box(wx, wy, ww, wh, OSD_ATTR_BORDER);
    osd_fill(wx + 1, wy + 1, ww - 2, wh - 2, ' ', OSD_ATTR_NORMAL);
    osd_print_center(wy + 2, "PCE Macintosh Plus", OSD_ATTR(OSD_YELLOW, OSD_BLUE));
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "Version %d.%d.%d",
             PCE_VERSION_MAJOR, PCE_VERSION_MINOR, PCE_VERSION_ITERATION);
    osd_print_center(wy + 4, version_str, OSD_ATTR_NORMAL);
    osd_print_center(wy + 5, "Hampa Hug <hampa@hampa.ch>. Port by Mike V73", OSD_ATTR_NORMAL);
    char hw_str[50];
    snprintf(hw_str, sizeof(hw_str), "RP2350 @ %d MHz / PSRAM @ %d MHz / FLASH @ %d MHz",
             config_get_cpu_freq(), config_get_psram_freq(), config_get_flash_freq());
    osd_print_center(wy + 7, hw_str, OSD_ATTR(OSD_LIGHTCYAN, OSD_BLUE));
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
    DBG_PRINT("Waiting for USB Serial connection...\n");
    absolute_time_t deadline = make_timeout_time_ms(USB_CONSOLE_DELAY_MS);
    while (!stdio_usb_connected() && !time_reached(deadline)) sleep_ms(100);
    DBG_PRINT(stdio_usb_connected() ? "USB Serial connected!\n\n" : "Timeout - continuing\n\n");
#else
    DBG_PRINT("USB HID mode: debug via UART\n\n");
#endif

    DBG_PRINT("Board Configuration:\n");
#ifdef BOARD_M1
    DBG_PRINT("  Board: M1\n");
#elif defined(BOARD_M2)
    DBG_PRINT("  Board: M2\n");
#else
    DBG_PRINT("  Board: Unknown\n");
#endif
    DBG_PRINT("  CPU Speed: %d MHz\n  PSRAM Speed: %d MHz\n\n", CPU_CLOCK_MHZ, PSRAM_MAX_FREQ_MHZ);

    if (!init_hardware()) {
        printf("\nHardware initialization failed!\n");
        while (true) sleep_ms(1000);
    }

    if (!init_emulator()) {
        printf("\nEmulator initialization failed!\n");
        while (true) sleep_ms(1000);
    }

    initialized = true;

    if (*(uint32_t*)(0x20000000 + (512ul << 10) - 32) != 0x1927fa52)
        show_welcome_screen();

    DBG_PRINT("\nStarting emulation...\n");

    // Main emulation loop (Core 0)
    while (true) {
        if (diskui_is_open() || settingsui_is_open()) {
            // UI mode: animate, poll keys, yield
            poll_keyboard();
            if (diskui_is_open())           diskui_animate();
            else if (settingsui_is_open())  settingsui_animate();
            sleep_ms(16);
        } else {
            // Emulation mode: run one instruction
            rp2350_mac_step();

            // Poll keyboard every ~2048 steps (~few ms at 7.8 MHz)
            static int poll_counter = 0;
            if (++poll_counter >= 2048) {
                poll_counter = 0;
                poll_keyboard();
            }
        }

        if (settingsui_restart_requested()) {
            settingsui_clear_restart();
            DBG_PRINT("Settings changed - triggering RP reset...\n");
            break;
        }
    }

    DBG_PRINT("\nEmulation stopped.\n");
    *(uint32_t*)(0x20000000 + (512ul << 10) - 32) = 0x1927fa52;
    watchdog_reboot(0, 0, 0);
    while (true);
    __unreachable();
    return 0;
}
