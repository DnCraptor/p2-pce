/**
 * Configuration Save - writes configuration to INI file on SD card.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include "config_save.h"
#include "board_config.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Hardware settings for RP2350 platform (use build-time defaults)
static int cfg_cpu_freq   = CPU_CLOCK_MHZ;
static int cfg_psram_freq = PSRAM_MAX_FREQ_MHZ;
static int cfg_flash_freq = FLASH_MAX_FREQ_MHZ;
static int cfg_volume     = 15;
static int cfg_voltage    = -1;  /* -1 = auto */
static bool cfg_changed    = false;
static bool cfg_hw_changed = false;

#define CONFIG_PATH "pce/config.ini"

void config_init_from_current(void) { cfg_changed = false; }

int  config_get_cpu_freq(void)   { return cfg_cpu_freq; }
void config_set_cpu_freq(int mhz) {
    if (cfg_cpu_freq != mhz) { cfg_cpu_freq = mhz; cfg_changed = true; cfg_hw_changed = true; }
}

int  config_get_psram_freq(void)    { return cfg_psram_freq; }
void config_set_psram_freq(int mhz) {
    if (cfg_psram_freq != mhz) { cfg_psram_freq = mhz; cfg_changed = true; cfg_hw_changed = true; }
}

int  config_get_flash_freq(void)    { return cfg_flash_freq; }
void config_set_flash_freq(int mhz) {
    if (cfg_flash_freq != mhz) { cfg_flash_freq = mhz; cfg_changed = true; cfg_hw_changed = true; }
}

int  config_get_volume(void)    { return cfg_volume; }
void config_set_volume(int vol) {
    if (cfg_volume != vol) { cfg_volume = vol; cfg_changed = true; }
}

int  config_get_voltage(void) { return cfg_voltage; }
void config_set_voltage(int v) {
    if (cfg_voltage != v) { cfg_voltage = v; cfg_changed = true; cfg_hw_changed = true; }
}

bool config_hw_changed(void)  { return cfg_hw_changed; }
bool config_has_changes(void) { return cfg_changed; }
void config_clear_changes(void) { cfg_changed = false; cfg_hw_changed = false; }

static bool write_line(FIL *fp, const char *line) {
    UINT bw;
    FRESULT res = f_write(fp, line, strlen(line), &bw);
    return (res == FR_OK && bw == strlen(line));
}

bool config_save_all(void) {
    FIL fp;
    FRESULT res;
    char line[80];

    res = f_open(&fp, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) return false;

    write_line(&fp, "[pce]\n");
    snprintf(line, sizeof(line), "cpu_freq=%d\n",   cfg_cpu_freq);   write_line(&fp, line);
    snprintf(line, sizeof(line), "psram_freq=%d\n", cfg_psram_freq); write_line(&fp, line);
    snprintf(line, sizeof(line), "flash_freq=%d\n", cfg_flash_freq); write_line(&fp, line);
    snprintf(line, sizeof(line), "volume=%d\n",     cfg_volume);     write_line(&fp, line);
    snprintf(line, sizeof(line), "voltage=%d\n",    cfg_voltage);    write_line(&fp, line);

    f_close(&fp);
    cfg_changed    = false;
    cfg_hw_changed = false;
    return true;
}

bool config_save_disks(void) {
    return config_save_all();
}

int parse_pce_ini(void* user, const char* section, const char* name, const char* value) {
    (void)user;
    if (strcmp(section, "pce") != 0) return 1;

    if      (strcmp(name, "cpu_freq")   == 0) cfg_cpu_freq   = atoi(value);
    else if (strcmp(name, "psram_freq") == 0) cfg_psram_freq = atoi(value);
    else if (strcmp(name, "flash_freq") == 0) cfg_flash_freq = atoi(value);
    else if (strcmp(name, "volume")     == 0) cfg_volume     = atoi(value);
    else if (strcmp(name, "voltage")    == 0) cfg_voltage    = atoi(value);

    return 1;
}
