/**
 * rp2350_stubs.c
 *
 * Заглушки для функций PCE, которые на RP2350 либо не нужны (sysdep),
 * либо заменяются платформенными аналогами.
 *
 * Принцип: минимум изменений в оригинальных исходниках PCE.
 * Все "нет на RP2350" реализуется здесь, а не патчами.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "debug.h"

/* ------------------------------------------------------------------ */
/* sysdep.h stubs                                                      */
/* ------------------------------------------------------------------ */

/* На RP2350 нет POSIX — заменяем на pico sleep */
int pce_usleep(unsigned long usec)
{
    sleep_us(usec);
    return 0;
}

/* Интервальный таймер через time_us_32 */
unsigned long pce_get_interval_us(unsigned long *val)
{
    unsigned long now = time_us_32();
    unsigned long delta = (now - (unsigned long)*val) & 0xffffffffUL;
    *val = now;
    return delta;
}

/* На RP2350 нет терминала — ничего не делаем */
void pce_set_fd_interactive(int fd, int interactive)
{
    (void)fd;
    (void)interactive;
}

int pce_fd_readable(int fd, int t)
{
    (void)fd;
    (void)t;
    return 0;
}

int pce_fd_writeable(int fd, int t)
{
    (void)fd;
    (void)t;
    return 0;
}

/* pce_start/pce_stop: в mac_run сбрасывают brk и настраивают tty.
 * На RP2350 только сброс brk нужен — это делается в mac_run самостоятельно. */
void pce_start(unsigned *brk)
{
    if (brk != NULL) {
        *brk = 0;
    }
}

void pce_stop(void)
{
    /* ничего */
}

/* ------------------------------------------------------------------ */
/* pce_log (log.h) → DBG_PRINT                                        */
/* ------------------------------------------------------------------ */
/* log.c не компилируется — там fopen/fclose на файловую систему хоста.
 * На RP2350 весь вывод идёт через DBG_PRINT → stdio (UART/USB). */

#define PCE_LOG_MAX 16

void pce_log_init(void) {}
void pce_log_done(void) {}

int pce_log_add_fp(void *fp, int close, unsigned level)
{
    (void)fp; (void)close; (void)level;
    return 0;
}

int pce_log_add_fname(const char *fname, unsigned level)
{
    (void)fname; (void)level;
    return 0;
}

void pce_log_rmv_fp(void *fp) { (void)fp; }
void pce_log_set_level(void *fp, unsigned level) { (void)fp; (void)level; }
unsigned pce_log_get_level(void *fp) { (void)fp; return 3; }

void pce_log(unsigned level, const char *msg, ...)
{
#ifdef DEBUG_ENABLED
    va_list va;
    va_start(va, msg);
    vprintf(msg, va);
    va_end(va);
#else
    (void)level; (void)msg;
#endif
}

void pce_log_va(unsigned level, const char *msg, va_list va)
{
#ifdef DEBUG_ENABLED
    vprintf(msg, va);
#else
    (void)level; (void)msg; (void)va;
#endif
}

void pce_log_deb(const char *msg, ...)
{
#ifdef DEBUG_ENABLED
    va_list va;
    va_start(va, msg);
    vprintf(msg, va);
    va_end(va);
#else
    (void)msg;
#endif
}

void pce_log_tag(unsigned level, const char *tag, const char *msg, ...)
{
#ifdef DEBUG_ENABLED
    va_list va;
    printf("[%s] ", tag);
    va_start(va, msg);
    vprintf(msg, va);
    va_end(va);
#else
    (void)level; (void)tag; (void)msg;
#endif
}

/* ------------------------------------------------------------------ */
/* console.h stubs (pce_puts / pce_console_*)                         */
/* ------------------------------------------------------------------ */
/* lib/console.c использует stdin/stdout для интерактивного монитора.
 * На RP2350 монитора нет — заглушаем. */

void pce_puts(const char *str)
{
    DBG_PRINT("%s", str);
}

void pce_printf(const char *fmt, ...)
{
#ifdef DEBUG_ENABLED
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
#else
    (void)fmt;
#endif
}

void pce_console_init(void *inp, void *out)
{
    (void)inp; (void)out;
}

void pce_console_done(void) {}

/* ------------------------------------------------------------------ */
/* path.h stubs                                                        */
/* ------------------------------------------------------------------ */
/* pce_path_set / pce_path_get используются в iniram.c / load.c для
 * поиска файлов относительно путей. На RP2350 всё лежит в pce/ на SD.
 * Возвращаем имя файла как есть — sdcard уже открывает из корня. */

void pce_path_set(const char *path) { (void)path; }
void pce_path_ini(void *ini)        { (void)ini;  }

char *pce_path_get(const char *name)
{
    if (name == NULL) return NULL;
    /* Возвращаем копию — вызывающий сделает free() */
    size_t len = strlen(name);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, name, len + 1);
    return copy;
}

/* ------------------------------------------------------------------ */
/* sound driver null stub                                              */
/* ------------------------------------------------------------------ */
/* mac_sound_set_driver(ms, NULL) в mac_setup_sound при driver==NULL
 * не вызывается — snd_open не будет дёргаться. Но snd_write(NULL,...)
 * уже проверяет sdrv==NULL и возвращает 1. Никаких доп. стабов не нужно.
 *
 * Когда придёт время интегрировать аудио — реализовать snd_rp2350_open()
 * в отдельном файле и передать имя "rp2350" в mac_setup_sound через INI:
 *   [sound]
 *   driver = rp2350
 */

/* ------------------------------------------------------------------ */
/* par_verbose (ожидается extern в некоторых arch-файлах)             */
/* ------------------------------------------------------------------ */
int par_verbose = 0;
