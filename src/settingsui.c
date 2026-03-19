/**
 * Settings UI - on-screen settings manager.
 * Triggered by Win+F11 hotkey.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include "settingsui.h"
#include "diskui.h"
#include "config_save.h"
#include "../drivers/vga/vga_osd.h"
#include <string.h>
#include <stdio.h>
#include "audio.h"

typedef enum {
    SETTINGS_CLOSED,
    SETTINGS_MAIN,
    SETTINGS_CONFIRM_SAVE_RESTART,
    SETTINGS_CONFIRM_SAVE
} SettingsState;

typedef enum {
    SETTING_VOL = 0,
    SETTING_CPU_FREQ,
    SETTING_VOLTAGE,
    SETTING_PSRAM_FREQ,
    SETTING_FLASH_FREQ,
    SETTING_COUNT
} SettingItem;

static const int vol_options[]  = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static const int vol_option_count = 17;

static const int cpu_freq_options[]  = { 252, 378, 504, 524, 564 };
static const int cpu_freq_option_count = 5;

static const int psram_freq_options[] = { 66, 84, 100, 133, 166 };
static const int psram_freq_option_count = 5;

static const int flash_freq_options[] = { 66, 84, 100, 133, 166 };
static const int flash_freq_option_count = 5;

static const int voltage_options[]    = { -1,     15,      16,      17,      18,      19,      20 };
static const char *voltage_labels[]   = { "Auto", "1.30V", "1.35V", "1.40V", "1.50V", "1.60V", "1.65V" };
static const int voltage_option_count = 7;

static SettingsState settings_state = SETTINGS_CLOSED;
static int selected_item = 0;
static int scroll_offset = 0;
static bool restart_requested = false;
static int plasma_frame = 0;

static int orig_cpu_freq, orig_psram_freq, orig_flash_freq, orig_volume, orig_voltage;

#define MENU_X      10
#define MENU_Y      5
#define MENU_W      60
#define MENU_H      (4 + SETTING_COUNT + 4)
#define VISIBLE_ITEMS SETTING_COUNT

static void draw_settings_menu(void);
static void draw_confirm_dialog(void);
static void draw_confirm_dialog2(void);
static int find_option_index(const int *options, int count, int value);
static void cycle_option(int direction);

void settingsui_init(void) {
    settings_state    = SETTINGS_CLOSED;
    restart_requested = false;
}

void settingsui_open(void) {
    if (settings_state != SETTINGS_CLOSED) return;
    orig_volume     = audio_get_volume();
    orig_cpu_freq   = config_get_cpu_freq();
    orig_psram_freq = config_get_psram_freq();
    orig_flash_freq = config_get_flash_freq();
    orig_voltage    = config_get_voltage();
    settings_state  = SETTINGS_MAIN;
    selected_item   = 0;
    scroll_offset   = 0;
    osd_clear();
    osd_show();
    draw_settings_menu();
}

void settingsui_close(void) {
    if (settings_state == SETTINGS_MAIN && config_has_changes()) {
        audio_set_volume(orig_volume);
        config_set_cpu_freq(orig_cpu_freq);
        config_set_psram_freq(orig_psram_freq);
        config_set_flash_freq(orig_flash_freq);
        config_set_voltage(orig_voltage);
        config_clear_changes();
    }
    settings_state = SETTINGS_CLOSED;
    osd_hide();
}

bool settingsui_is_open(void)         { return settings_state != SETTINGS_CLOSED; }
bool settingsui_restart_requested(void) { return restart_requested; }
void settingsui_clear_restart(void)   { restart_requested = false; }

static int find_option_index(const int *options, int count, int value) {
    for (int i = 0; i < count; i++) if (options[i] == value) return i;
    return 0;
}

static void cycle_option(int direction) {
    int idx, count;
    const int *options;
    switch (selected_item) {
        case SETTING_VOL:
            options = vol_options; count = vol_option_count;
            idx = find_option_index(options, count, audio_get_volume());
            idx = (idx + direction + count) % count;
            audio_set_volume(options[idx]);
            config_set_volume(options[idx]);
            break;
        case SETTING_CPU_FREQ:
            options = cpu_freq_options; count = cpu_freq_option_count;
            idx = find_option_index(options, count, config_get_cpu_freq());
            idx = (idx + direction + count) % count;
            config_set_cpu_freq(options[idx]);
            break;
        case SETTING_VOLTAGE:
            options = voltage_options; count = voltage_option_count;
            idx = find_option_index(options, count, config_get_voltage());
            idx = (idx + direction + count) % count;
            config_set_voltage(options[idx]);
            break;
        case SETTING_PSRAM_FREQ:
            options = psram_freq_options; count = psram_freq_option_count;
            idx = find_option_index(options, count, config_get_psram_freq());
            idx = (idx + direction + count) % count;
            config_set_psram_freq(options[idx]);
            break;
        case SETTING_FLASH_FREQ:
            options = flash_freq_options; count = flash_freq_option_count;
            idx = find_option_index(options, count, config_get_flash_freq());
            idx = (idx + direction + count) % count;
            config_set_flash_freq(options[idx]);
            break;
    }
}

static void draw_settings_menu(void) {
    osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);
    osd_draw_box(MENU_X, MENU_Y, MENU_W, MENU_H, OSD_ATTR_BORDER);
    osd_fill(MENU_X + 1, MENU_Y + 1, MENU_W - 2, MENU_H - 2, ' ', OSD_ATTR_NORMAL);
    osd_print_center(MENU_Y, " Settings ", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    const char *labels[] = {
        "Volume:",
        "RP2350 Freq:",
        "CPU Voltage:",
        "PSRAM Freq:",
        "Flash Freq:"
    };
    char value[24];

    for (int i = 0; i < SETTING_COUNT; i++) {
        int y = MENU_Y + 2 + i;
        uint8_t attr = (i == selected_item) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;
        osd_fill(MENU_X + 2, y, MENU_W - 4, 1, ' ', attr);
        osd_print(MENU_X + 3, y, labels[i], attr);
        switch (i) {
            case SETTING_VOL:
                snprintf(value, sizeof(value), "< %d >", audio_get_volume()); break;
            case SETTING_CPU_FREQ:
                snprintf(value, sizeof(value), "< %d MHz >", config_get_cpu_freq()); break;
            case SETTING_VOLTAGE: {
                int idx = find_option_index(voltage_options, voltage_option_count, config_get_voltage());
                snprintf(value, sizeof(value), "< %s >", voltage_labels[idx]); break;
            }
            case SETTING_PSRAM_FREQ:
                snprintf(value, sizeof(value), "< %d MHz >", config_get_psram_freq()); break;
            case SETTING_FLASH_FREQ:
                snprintf(value, sizeof(value), "< %d MHz >", config_get_flash_freq()); break;
        }
        osd_print(MENU_X + 26, y, value, attr);
    }

    if (config_has_changes())
        osd_print(MENU_X + 3, MENU_Y + MENU_H - 4, "* Changes pending - Enter to save+restart", OSD_ATTR_HIGHLIGHT);

    osd_print(MENU_X + 2, MENU_Y + MENU_H - 2,
              "\x18\x19:Select  \x1b\x1a:Change  Enter:Apply  Esc:Cancel",
              OSD_ATTR_HIGHLIGHT);
}

static void draw_confirm_dialog(void) {
    int dx = 20, dy = 10, dw = 40, dh = 5;
    uint8_t da = OSD_ATTR(OSD_WHITE, OSD_RED);
    osd_draw_box(dx, dy, dw, dh, da);
    osd_fill(dx+1, dy+1, dw-2, dh-2, ' ', da);
    osd_print(dx+3, dy+2, "Save settings and restart? (Y/N)", da);
}

static void draw_confirm_dialog2(void) {
    int dx = 20, dy = 10, dw = 40, dh = 5;
    uint8_t da = OSD_ATTR(OSD_WHITE, OSD_RED);
    osd_draw_box(dx, dy, dw, dh, da);
    osd_fill(dx+1, dy+1, dw-2, dh-2, ' ', da);
    osd_print(dx+3, dy+2, "Discard changes and close? (Y/N)", da);
}

bool settingsui_handle_key(int keycode, bool is_down) {
    if (!is_down) return true;
    switch (settings_state) {
        case SETTINGS_MAIN:
            switch (keycode) {
                case KEY_UP:
                    selected_item = (selected_item > 0) ? selected_item - 1 : SETTING_COUNT - 1;
                    draw_settings_menu(); break;
                case KEY_DOWN:
                    selected_item = (selected_item < SETTING_COUNT - 1) ? selected_item + 1 : 0;
                    draw_settings_menu(); break;
                case KEY_LEFT:  cycle_option(-1); draw_settings_menu(); break;
                case KEY_RIGHT: cycle_option(+1); draw_settings_menu(); break;
                case KEY_ENTER:
                    if (config_has_changes()) {
                        settings_state = SETTINGS_CONFIRM_SAVE_RESTART;
                        draw_confirm_dialog();
                    } else settingsui_close();
                    break;
                case KEY_ESC:
                    if (config_has_changes()) {
                        settings_state = SETTINGS_CONFIRM_SAVE;
                        draw_confirm_dialog2();
                    } else settingsui_close();
                    break;
            }
            break;
        case SETTINGS_CONFIRM_SAVE_RESTART:
            if (keycode == 21) {  // Y
                config_save_all();
                restart_requested = true;
                settings_state = SETTINGS_CLOSED;
                osd_hide();
            } else if (keycode == 49 || keycode == KEY_ESC) {
                settings_state = SETTINGS_MAIN;
                draw_settings_menu();
            }
            break;
        case SETTINGS_CONFIRM_SAVE:
            if (keycode == 21) {  // Y
                settingsui_close();
            } else if (keycode == 49 || keycode == KEY_ESC) {
                settings_state = SETTINGS_MAIN;
                draw_settings_menu();
            }
            break;
        default: break;
    }
    return true;
}

void settingsui_animate(void) {
    if (settings_state == SETTINGS_CLOSED) return;
    plasma_frame++;
    osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);
}
