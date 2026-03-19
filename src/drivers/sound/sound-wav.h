/* drivers/sound/sound-wav.h — stub for RP2350.
 * WAV recording not needed on bare metal target. */
#ifndef PCE_DRIVERS_SOUND_WAV_H
#define PCE_DRIVERS_SOUND_WAV_H 1

#include <drivers/sound/sound.h>

static inline int  snd_wav_init       (sound_drv_t *s, const char *n) { (void)s;(void)n; return 0; }
static inline void snd_wav_close      (sound_drv_t *s)                { (void)s; }
static inline int  snd_wav_set_params (sound_drv_t *s, unsigned c,
                                       unsigned long r, int g)        { (void)s;(void)c;(void)r;(void)g; return 0; }
static inline int  snd_wav_write      (sound_drv_t *s,
                                       const uint16_t *b, unsigned n) { (void)s;(void)b;(void)n; return 0; }
#endif
