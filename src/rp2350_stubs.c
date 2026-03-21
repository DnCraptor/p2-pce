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

#include <ff.h>
#include <stdarg.h>
static FIL _2f_tf;
static int _2f_tf_open = 0;
void debug_log(const char *fmt, ...) {
    if (!_2f_tf_open) {
        _2f_tf_open = (f_open(&_2f_tf, "pce/log.txt", FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK);
    }
    if (!_2f_tf_open) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    UINT bw;
    f_write(&_2f_tf, buf, len, &bw);
    f_sync(&_2f_tf);
}

/* ------------------------------------------------------------------ */
/* Вспомогательная функция: DBG_PRINT через va_list                   */
/* ------------------------------------------------------------------ */
/* DBG_PRINT принимает только compile-time fmt. Для функций которые
 * получают va_list, форматируем в буфер и передаём как строку. */

static void dbg_vprint(const char *fmt, va_list va)
{
#ifdef DEBUG_ENABLED
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, va);
    DBG_PRINT("%s", buf);
#else
    (void)fmt; (void)va;
#endif
}

/* ------------------------------------------------------------------ */
/* sysdep.h stubs                                                      */
/* ------------------------------------------------------------------ */

int pce_usleep(unsigned long usec)
{
    sleep_us(usec);
    return 0;
}

unsigned long pce_get_interval_us(unsigned long *val)
{
    unsigned long now = time_us_32();
    unsigned long delta = (now - (unsigned long)*val) & 0xffffffffUL;
    *val = now;
    return delta;
}

void pce_set_fd_interactive(int fd, int interactive)
{
    (void)fd; (void)interactive;
}

int pce_fd_readable(int fd, int t)
{
    (void)fd; (void)t;
    return 0;
}

int pce_fd_writeable(int fd, int t)
{
    (void)fd; (void)t;
    return 0;
}

void pce_start(unsigned *brk)
{
    if (brk != NULL) *brk = 0;
}

void pce_stop(void) {}

/* ------------------------------------------------------------------ */
/* pce_log (log.h) → DBG_PRINT                                        */
/* ------------------------------------------------------------------ */

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

void pce_log_rmv_fp(void *fp)                      { (void)fp; }
void pce_log_set_level(void *fp, unsigned level)   { (void)fp; (void)level; }
unsigned pce_log_get_level(void *fp)               { (void)fp; return 3; }

void pce_log(unsigned level, const char *msg, ...)
{
    (void)level;
    va_list va;
    va_start(va, msg);
    dbg_vprint(msg, va);
    va_end(va);
}

void pce_log_va(unsigned level, const char *msg, va_list va)
{
    (void)level;
    dbg_vprint(msg, va);
}

void pce_log_deb(const char *msg, ...)
{
    va_list va;
    va_start(va, msg);
    dbg_vprint(msg, va);
    va_end(va);
}

void pce_log_tag(unsigned level, const char *tag, const char *msg, ...)
{
    (void)level;
    DBG_PRINT("[%s] ", tag);
    va_list va;
    va_start(va, msg);
    dbg_vprint(msg, va);
    va_end(va);
}

/* ------------------------------------------------------------------ */
/* console.h stubs                                                     */
/* ------------------------------------------------------------------ */

void pce_puts(const char *str)
{
    DBG_PRINT("%s", str);
}

void pce_printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    dbg_vprint(fmt, va);
    va_end(va);
}

void pce_console_init(void *inp, void *out) { (void)inp; (void)out; }
void pce_console_done(void) {}

/* ------------------------------------------------------------------ */
/* path.h stubs                                                        */
/* ------------------------------------------------------------------ */

void pce_path_set(const char *path) { (void)path; }
void pce_path_ini(void *ini)        { (void)ini; }

char *pce_path_get(const char *name)
{
    if (name == NULL) return NULL;
    size_t len = strlen(name);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, name, len + 1);
    return copy;
}

/* ------------------------------------------------------------------ */
/* par_verbose                                                         */
/* ------------------------------------------------------------------ */
int par_verbose = 0;
