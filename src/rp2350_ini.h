/**
 * rp2350_ini.h / .c — загрузчик INI для RP2350
 *
 * Читает pce/config.ini с SD карты и строит ini_sct_t,
 * который передаётся в mac_new(). Изолирует libini от fatfs.
 *
 * Использование:
 *   ini_sct_t *cfg = rp2350_load_ini("pce/config.ini", &err);
 *   par_sim = mac_new(cfg);
 *   ini_sct_del(cfg);   // после mac_new — cfg больше не нужен
 */

#ifndef RP2350_INI_H
#define RP2350_INI_H

#include <libini/libini.h>

/**
 * Загружает INI файл с SD карты и возвращает корневую ini_sct_t.
 * Вызывать после f_mount(). При ошибке возвращает NULL.
 * Вызывающий владеет памятью — освободить через ini_sct_del().
 */
ini_sct_t *rp2350_load_ini(const char *path, const char** err);

#endif /* RP2350_INI_H */
