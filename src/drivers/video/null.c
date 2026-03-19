/**
 * null.c — null terminal driver stub for RP2350.
 * initerm.c calls null_new() when driver="null".
 * Headless mode: always returns NULL so sim->trm stays NULL.
 * mac_video_update() already guards: if (mv->trm == NULL) return;
 */

#include <drivers/video/terminal.h>
#include <libini/libini.h>

terminal_t *null_new(ini_sct_t *ini)
{
    (void)ini;
    return NULL;  /* headless: no terminal */
}
