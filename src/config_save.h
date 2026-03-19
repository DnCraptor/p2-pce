/**
 * Configuration Save - writes configuration to INI file on SD card.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#ifndef CONFIG_SAVE_H
#define CONFIG_SAVE_H

#include <stdint.h>
#include <stdbool.h>

bool config_save_all(void);
bool config_save_disks(void);

// RP2350 hardware parameters
int  config_get_cpu_freq(void);    void config_set_cpu_freq(int mhz);
int  config_get_psram_freq(void);  void config_set_psram_freq(int mhz);
int  config_get_flash_freq(void);  void config_set_flash_freq(int mhz);
int  config_get_volume(void);      void config_set_volume(int vol);
int  config_get_voltage(void);     void config_set_voltage(int v);

bool config_hw_changed(void);
bool config_has_changes(void);
void config_clear_changes(void);
void config_init_from_current(void);

// INI parser callback for [pce] section (called from ini_parse_string)
int parse_pce_ini(void* user, const char* section,
                  const char* name, const char* value);

#endif // CONFIG_SAVE_H
