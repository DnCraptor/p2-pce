/**
 * null.c — null terminal driver stub for RP2350.
 * initerm.c calls null_new() when driver="null".
 * Headless mode: always returns NULL so sim->trm stays NULL.
 * mac_video_update() already guards: if (mv->trm == NULL) return;
 */

#include <drivers/video/terminal.h>
#include <libini/libini.h>
#include <stdlib.h>
#include "debug.h"

static
void del (void* p) {

}

static
int open (void* p, unsigned w, unsigned h) {
    return 0;
}

static
int close (void* p) {
    return 0;
}

static
int set_msg_trm (void* p, const char *msg, const char *val) {
    return 0;
}

static
void update (void* p) {

}

static
void check (void* p) {

}

terminal_t* null_new(ini_sct_t *ini)
{
    (void)ini;
    terminal_t* res = calloc(sizeof(terminal_t), 1);
    if (!res) {
        return NULL;
    }

	res->del = (void *) del;
	res->open = (void *) open;
	res->close = (void *) close;
	res->set_msg_trm = (void *) set_msg_trm;
	res->update = (void *) update;
	res->check = (void *) check;

    return res;
}
