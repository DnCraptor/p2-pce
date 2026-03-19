/**
 * rp2350_ini.c — загрузчик INI для RP2350
 */

#include "rp2350_ini.h"
#include "debug.h"

#include "ff.h"

#include <stdlib.h>
#include <string.h>

ini_sct_t *rp2350_load_ini(const char *path)
{
    FIL     fp;
    FRESULT res;
    FSIZE_t size;
    char   *buf;
    UINT    bytes_read;

    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) {
        DBG_PRINT("rp2350_ini: cannot open %s (err %d)\n", path, res);
        return NULL;
    }

    size = f_size(&fp);
    buf  = malloc((size_t)size + 1);
    if (!buf) {
        f_close(&fp);
        DBG_PRINT("rp2350_ini: OOM (%lu bytes)\n", (unsigned long)size);
        return NULL;
    }

    res = f_read(&fp, buf, (UINT)size, &bytes_read);
    f_close(&fp);

    if (res != FR_OK || bytes_read != (UINT)size) {
        DBG_PRINT("rp2350_ini: read error (err %d, got %u of %lu)\n",
                  res, bytes_read, (unsigned long)size);
        free(buf);
        return NULL;
    }
    buf[size] = '\0';

    ini_sct_t *root = ini_sct_new(NULL);
    if (!root) {
        free(buf);
        return NULL;
    }

    if (ini_read_str(root, buf)) {
        DBG_PRINT("rp2350_ini: parse error in %s\n", path);
        ini_sct_del(root);
        free(buf);
        return NULL;
    }

    free(buf);
    DBG_PRINT("rp2350_ini: loaded %s (%lu bytes)\n", path, (unsigned long)size);
    return root;
}
