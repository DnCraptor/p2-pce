/*****************************************************************************
 * pce                                                                       *
 *****************************************************************************/

/*****************************************************************************
 * File name:   src/arch/macplus/video.c                                     *
 * Created:     2007-04-16 by Hampa Hug <hampa@hampa.ch>                     *
 * Copyright:   (C) 2007-2012 Hampa Hug <hampa@hampa.ch>                     *
 *****************************************************************************/

/*****************************************************************************
 * This program is free software. You can redistribute it and / or modify it *
 * under the terms of the GNU General Public License version 2 as  published *
 * by the Free Software Foundation.                                          *
 *                                                                           *
 * This program is distributed in the hope  that  it  will  be  useful,  but *
 * WITHOUT  ANY   WARRANTY,   without   even   the   implied   warranty   of *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU  General *
 * Public License for more details.                                          *
 *****************************************************************************/


#include "main.h"
#include "video.h"

#include <stdlib.h>
#include <string.h>


#define MAC_VIDEO_PFREQ 15667200
#define MAC_VIDEO_HFREQ (MAC_VIDEO_PFREQ / (512 + 192))
#define MAC_VIDEO_VFREQ (MAC_VIDEO_HFREQ / (342 + 28))

/* #define MAC_VIDEO_VB1 ((342 * (512 + 192) * 7833600) / MAC_VIDEO_PFREQ) */
#define MAC_VIDEO_VB1 120384

/* #define MAC_VIDEO_VB2 (((342 + 28) * (512 + 192) * 7833600) / MAC_VIDEO_PFREQ) */
#define MAC_VIDEO_VB2 130240


int mac_video_init (mac_video_t *mv, unsigned w, unsigned h)
{
	mv->vbuf = NULL;
	mv->trm = NULL;

	mv->w = w;
	mv->h = h;

	mv->force = 0;

	mv->cmp_cnt = 8;

	mv->vcmp = malloc ((unsigned long) ((mv->w + 7) / 8) * mv->h);

	if (mv->vcmp == NULL) {
		return (1);
	}

	mv->rgb = malloc (3UL * (unsigned long) w * mv->cmp_cnt);

	if (mv->rgb == NULL) {
		return (1);
	}

	mv->brightness = 255;

	mv->col0[0] = 0;
	mv->col0[1] = 0;
	mv->col0[2] = 0;

	mv->col1[0] = 0xff;
	mv->col1[1] = 0xff;
	mv->col1[2] = 0xff;

	mv->clk = 0;

	mv->vbi_val = 0;
	mv->vbi_ext = NULL;
	mv->set_vbi = NULL;

	return (0);
}

mac_video_t *mac_video_new (unsigned w, unsigned h)
{
	mac_video_t *mv;

	mv = malloc (sizeof (mac_video_t));

	if (mv == NULL) {
		return (NULL);
	}

	if (mac_video_init (mv, w, h)) {
		free (mv);
		return (NULL);
	}

	return (mv);
}

void mac_video_free (mac_video_t *mv)
{
}

void mac_video_del (mac_video_t *mv)
{
	if (mv != NULL) {
		mac_video_free (mv);
		free (mv);
	}
}

void mac_video_set_vbi_fct (mac_video_t *mv, void *ext, void *fct)
{
	mv->vbi_ext = ext;
	mv->set_vbi = fct;
}

void mac_video_set_vbuf (mac_video_t *mv, const unsigned char *vbuf)
{
	mv->vbuf = vbuf;
}

void mac_video_set_terminal (mac_video_t *mv, terminal_t *trm)
{
	mv->trm = trm;

	if (mv->trm != NULL) {
		trm_open (mv->trm, mv->w, mv->h);
		trm_set_size (mv->trm, mv->w, mv->h);
	}
}

void mac_video_set_color (mac_video_t *mv, unsigned long col0, unsigned long col1)
{
	unsigned i;

	for (i = 0; i < 3; i++) {
		mv->col0[i] = (col0 >> (8 * (2 - i))) & 0xff;
		mv->col1[i] = (col1 >> (8 * (2 - i))) & 0xff;
	}

	mv->force = 1;
}

void mac_video_set_brightness (mac_video_t *mv, unsigned val)
{
	if (val > 255) {
		val = 255;
	}

	if (mv->brightness != val) {
		mv->force = 1;
		mv->brightness = val;
	}
}

static
void mac_video_set_vbi (mac_video_t *mv, unsigned char val)
{
	if (mv->vbi_val == val) {
		return;
	}

	mv->vbi_val = val;

	if (mv->set_vbi != NULL) {
		mv->set_vbi (mv->vbi_ext, val);
	}
}

static
void mac_video_update (mac_video_t *mv)
{
	if (mv->trm == NULL) {
		return;
	}

	if (mv->trm->buf == NULL) {
		trm_set_size(mv->trm, mv->w, mv->h);
	}

	if (mv->vbuf == NULL) {
		return;
	}

	// copy to fast SRAM
	memcpy(mv->trm->buf, mv->vbuf, mv->w * mv->h / 8);

	mv->force = 0;

	trm_update (mv->trm);
}

void mac_video_redraw (mac_video_t *mv)
{
	mac_video_update (mv);
}

void mac_video_clock (mac_video_t *mv, unsigned long n)
{
	unsigned long old;

	old = mv->clk;

	mv->clk += n;

	if (mv->clk < MAC_VIDEO_VB1) {
		return;
	}

	if (old < MAC_VIDEO_VB1) {
		/* vbl start */
		mac_video_update (mv);
		mac_video_set_vbi (mv, 1);
	}

	if (mv->clk >= MAC_VIDEO_VB2) {
		mac_video_set_vbi (mv, 0);

		mv->clk -= MAC_VIDEO_VB2;
	}
}
