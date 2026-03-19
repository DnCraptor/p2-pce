/**
 * snd_null.c — null sound driver stub for RP2350.
 * snd_null_open is referenced by sound.c driver table.
 * Returns a driver that discards all audio output.
 */
#include <stdlib.h>
#include <stdint.h>
#include <drivers/sound/sound.h>

static void null_close (sound_drv_t *s) { free(s); }
static int  null_write (sound_drv_t *s, const uint16_t *b, unsigned n) { (void)s;(void)b;(void)n; return 0; }
static int  null_params(sound_drv_t *s, unsigned c, unsigned long r, int g) { (void)s;(void)c;(void)r;(void)g; return 0; }

sound_drv_t *snd_null_open(const char *name) {
    (void)name;
    sound_drv_t *s = malloc(sizeof(sound_drv_t));
    if (!s) return NULL;
    snd_init(s, NULL);
    s->close      = null_close;
    s->write      = null_write;
    s->set_params = null_params;
    return s;
}
