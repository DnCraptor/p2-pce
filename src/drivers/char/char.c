/**
 * char.c — character driver for RP2350.
 *
 * Реализует весь API из char.h. На RP2350 нет POSIX serial,
 * поэтому все драйверы кроме null возвращают null-драйвер.
 * Mac SCC (serial chip) эмулируется, но данные просто дропаются.
 */

#include <stdlib.h>
#include <string.h>
#include <drivers/char/char.h>
#include <drivers/options.h>

/* ------------------------------------------------------------------ */
/* Null driver callbacks                                               */
/* ------------------------------------------------------------------ */

static void     null_close      (char_drv_t *d) { free(d); }
static unsigned null_read       (char_drv_t *d, void *b, unsigned n)  { (void)d;(void)b; return 0; }
static unsigned null_write      (char_drv_t *d, const void *b, unsigned n) { (void)d;(void)b; return n; }
static int      null_get_ctl    (char_drv_t *d, unsigned *c)          { (void)d; *c = 0; return 0; }
static int      null_set_ctl    (char_drv_t *d, unsigned c)           { (void)d;(void)c; return 0; }
static int      null_set_params (char_drv_t *d, unsigned long b, unsigned c, unsigned p, unsigned s)
                                                                       { (void)d;(void)b;(void)c;(void)p;(void)s; return 0; }

static char_drv_t *make_null(void) {
    char_drv_t *d = malloc(sizeof(char_drv_t));
    if (!d) return NULL;
    chr_init(d, NULL);
    d->close      = null_close;
    d->read       = null_read;
    d->write      = null_write;
    d->get_ctl    = null_get_ctl;
    d->set_ctl    = null_set_ctl;
    d->set_params = null_set_params;
    return d;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void chr_init(char_drv_t *cdrv, void *ext) {
    memset(cdrv, 0, sizeof(char_drv_t));
    cdrv->ext = ext;
}

void chr_close(char_drv_t *cdrv) {
    if (cdrv && cdrv->close) cdrv->close(cdrv);
}

unsigned chr_read(char_drv_t *cdrv, void *buf, unsigned cnt) {
    if (cdrv && cdrv->read) return cdrv->read(cdrv, buf, cnt);
    return 0;
}

unsigned chr_write(char_drv_t *cdrv, const void *buf, unsigned cnt) {
    if (cdrv && cdrv->write) return cdrv->write(cdrv, buf, cnt);
    return cnt;
}

int chr_get_ctl(char_drv_t *cdrv, unsigned *ctl) {
    if (cdrv && cdrv->get_ctl) return cdrv->get_ctl(cdrv, ctl);
    if (ctl) *ctl = 0;
    return 0;
}

int chr_set_ctl(char_drv_t *cdrv, unsigned ctl) {
    if (cdrv && cdrv->set_ctl) return cdrv->set_ctl(cdrv, ctl);
    return 0;
}

int chr_set_params(char_drv_t *cdrv, unsigned long bps, unsigned bpc,
                   unsigned parity, unsigned stop) {
    if (cdrv && cdrv->set_params) return cdrv->set_params(cdrv, bps, bpc, parity, stop);
    return 0;
}

int chr_set_log(char_drv_t *cdrv, const char *fname) {
    (void)cdrv; (void)fname;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver table                                                        */
/* ------------------------------------------------------------------ */

char_drv_t *chr_open(const char *name)       { (void)name; return make_null(); }
char_drv_t *chr_null_open(const char *name)  { (void)name; return make_null(); }
char_drv_t *chr_mouse_open(const char *name) { (void)name; return make_null(); }
char_drv_t *chr_stdio_open(const char *name) { (void)name; return make_null(); }
char_drv_t *chr_posix_open(const char *name) { (void)name; return make_null(); }
char_drv_t *chr_ppp_open(const char *name)   { (void)name; return make_null(); }
char_drv_t *chr_pty_open(const char *name)   { (void)name; return make_null(); }
char_drv_t *chr_slip_open(const char *name)  { (void)name; return make_null(); }
char_drv_t *chr_tcp_open(const char *name)   { (void)name; return make_null(); }
char_drv_t *chr_tios_open(const char *name)  { (void)name; return make_null(); }

void chr_mouse_set(int dx, int dy, unsigned button) {
    (void)dx; (void)dy; (void)button;
}
