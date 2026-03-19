/**
 * pfdc-img.c — stub for RP2350.
 *
 * Original pfdc-img.c dispatches to 10 format drivers (AnadiskE, CP2, DC42,
 * IMD, PFDC, RAW, TC, TD0, XDF, ...) which are not in this repository.
 *
 * For Mac 128K/Plus we only need raw GCR disk images (.img/.dsk).
 * blkfdc.c calls pfdc_load_fp / pfdc_save_fp with type=PFDC_FORMAT_RAW
 * (or probed). Everything else returns NULL / error.
 *
 * pfdc-img-raw.h/c are also absent, so we inline the minimal raw loader:
 * a raw disk image is simply a flat array of 512-byte sectors, C/H/S order.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pfdc.h"
#include "pfdc-img.h"

/* ------------------------------------------------------------------ */
/* Guess format from filename extension                                */
/* ------------------------------------------------------------------ */

unsigned pfdc_guess_type(const char *fname)
{
    if (fname == NULL) return PFDC_FORMAT_NONE;
    const char *ext = strrchr(fname, '.');
    if (ext == NULL) return PFDC_FORMAT_RAW;
    if (strcasecmp(ext, ".img") == 0) return PFDC_FORMAT_RAW;
    if (strcasecmp(ext, ".dsk") == 0) return PFDC_FORMAT_RAW;
    if (strcasecmp(ext, ".ima") == 0) return PFDC_FORMAT_RAW;
    return PFDC_FORMAT_RAW;   /* assume raw for anything unknown */
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

unsigned pfdc_probe_fp(FILE *fp) { (void)fp; return PFDC_FORMAT_RAW; }

unsigned pfdc_probe(const char *fname)
{
    FILE *fp = fopen(fname, "rb");
    if (!fp) return PFDC_FORMAT_NONE;
    fclose(fp);
    return pfdc_guess_type(fname);
}

/* ------------------------------------------------------------------ */
/* Raw loader: flat 512-byte sectors, C×H×S order                     */
/* ------------------------------------------------------------------ */

static pfdc_img_t *pfdc_load_raw_fp(FILE *fp)
{
    /* Determine file size */
    if (fseek(fp, 0, SEEK_END) != 0) return NULL;
    long size = ftell(fp);
    rewind(fp);
    if (size <= 0 || (size % 512) != 0) return NULL;

    unsigned total_sectors = (unsigned)(size / 512);

    /* Mac 400K: 80 tracks, 1 head, variable spt (GCR).
     * Mac 800K: 80 tracks, 2 heads.
     * For a flat raw image we map as: c=0..79, h=0..heads-1, s=1..spt
     * Use simple geometry: try 2-sided 800K first, then 400K. */
    unsigned heads = (total_sectors > 800) ? 2 : 1;
    unsigned cyls  = 80;
    unsigned spt   = total_sectors / (cyls * heads);
    if (spt == 0) spt = 1;

    pfdc_img_t *img = pfdc_img_new();
    if (!img) return NULL;

    unsigned char buf[512];
    for (unsigned c = 0; c < cyls; c++) {
        for (unsigned h = 0; h < heads; h++) {
            for (unsigned s = 1; s <= spt; s++) {
                if (fread(buf, 1, 512, fp) != 512) {
                    pfdc_img_del(img);
                    return NULL;
                }
                pfdc_sct_t *sct = pfdc_sct_new(c, h, s, 512);
                if (!sct) { pfdc_img_del(img); return NULL; }
                memcpy(sct->data, buf, 512);
                pfdc_img_add_sector(img, sct, c, h);
            }
        }
    }
    return img;
}

/* ------------------------------------------------------------------ */
/* Raw saver                                                           */
/* ------------------------------------------------------------------ */

static int pfdc_save_raw_fp(FILE *fp, const pfdc_img_t *img)
{
    /* Write sectors in C/H/S order */
    for (unsigned c = 0; c < img->cyl_cnt; c++) {
        pfdc_cyl_t *cyl = img->cyl[c];
        if (!cyl) continue;
        for (unsigned h = 0; h < cyl->trk_cnt; h++) {
            pfdc_trk_t *trk = cyl->trk[h];
            if (!trk) continue;
            for (unsigned s = 0; s < trk->sct_cnt; s++) {
                pfdc_sct_t *sct = trk->sct[s];
                if (!sct) continue;
                if (fwrite(sct->data, 1, sct->n, fp) != sct->n) return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

pfdc_img_t *pfdc_load_fp(FILE *fp, unsigned type)
{
    (void)type;
    return pfdc_load_raw_fp(fp);
}

pfdc_img_t *pfdc_load(const char *fname, unsigned type)
{
    FILE *fp = fopen(fname, "rb");
    if (!fp) return NULL;
    pfdc_img_t *img = pfdc_load_fp(fp, type);
    fclose(fp);
    return img;
}

int pfdc_save_fp(FILE *fp, const pfdc_img_t *img, unsigned type)
{
    (void)type;
    return pfdc_save_raw_fp(fp, img);
}

int pfdc_save(const char *fname, const pfdc_img_t *img, unsigned type)
{
    FILE *fp = fopen(fname, "wb");
    if (!fp) return 1;
    int r = pfdc_save_fp(fp, img, type);
    fclose(fp);
    return r;
}
