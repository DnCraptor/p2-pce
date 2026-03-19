/**
 * char_stubs.c — stub implementations for RP2350.
 * chr_null_open, chr_mouse_open, chr_stdio_open are referenced by char.c
 * but their source files don't exist in this repo. On RP2350 serial ports
 * use chr_null (no I/O) — the Mac serial chip is emulated but no host port.
 */
#include <stdlib.h>
#include <drivers/char/char.h>

/* Shared null driver implementation */
static void       null_close      (char_drv_t *d) { free(d); }
static unsigned   null_read       (char_drv_t *d, void *b, unsigned n)                          { (void)d;(void)b; return 0; }
static unsigned   null_write      (char_drv_t *d, const void *b, unsigned n)                    { (void)d;(void)b; return n; }
static int        null_get_ctl    (char_drv_t *d, unsigned *c)                                  { (void)d; *c=0; return 0; }
static int        null_set_ctl    (char_drv_t *d, unsigned c)                                   { (void)d;(void)c; return 0; }
static int        null_set_params (char_drv_t *d, unsigned long b, unsigned c, unsigned p, unsigned s) { (void)d;(void)b;(void)c;(void)p;(void)s; return 0; }

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

char_drv_t *chr_null_open  (const char *name) { (void)name; return make_null(); }
char_drv_t *chr_mouse_open (const char *name) { (void)name; return make_null(); }
char_drv_t *chr_stdio_open (const char *name) { (void)name; return make_null(); }

/* Stubs for disabled POSIX drivers — char.c references these via #ifdef guards
 * that are disabled, but they appear in the driver table conditionally.
 * Defined here as NULL so the linker finds symbols if somehow referenced. */
void chr_mouse_set(int dx, int dy, unsigned button) { (void)dx;(void)dy;(void)button; }
