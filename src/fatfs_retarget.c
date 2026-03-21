/**
 * fatfs_retarget.c — перенаправление стандартных FILE* функций на FatFS.
 *
 * Использует механизм GNU linker --wrap.
 * stdin/stdout/stderr пропускаются напрямую.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "ff.h"
#include "debug.h"

/* ------------------------------------------------------------------ */
/* FILE* → FIL mapping                                                */
/* ------------------------------------------------------------------ */

#define FATFS_MAX_FILES 16

typedef struct {
    FIL  fil;
    int  used;
    int  eof;
    int  error;
} fatfs_file_t;

static fatfs_file_t _files[FATFS_MAX_FILES];

#define IS_STDIO(fp) ((fp) == stdin || (fp) == stdout || (fp) == stderr)

static fatfs_file_t *fp_to_fat(FILE *fp)
{
    if (!fp || IS_STDIO(fp)) return NULL;
    fatfs_file_t *f = (fatfs_file_t *)fp;
    if (f < _files || f >= _files + FATFS_MAX_FILES) return NULL;
    if (!f->used) return NULL;
    return f;
}

static FILE *fat_to_fp(fatfs_file_t *f)
{
    return (FILE *)f;
}

/* ------------------------------------------------------------------ */
/* __wrap_fopen                                                        */
/* ------------------------------------------------------------------ */

FILE *__wrap_fopen(const char *path, const char *mode)
{
    fatfs_file_t *f = NULL;
    for (int i = 0; i < FATFS_MAX_FILES; i++) {
        if (!_files[i].used) { f = &_files[i]; break; }
    }
    if (!f) {
        DBG_PRINT("fatfs: fopen(%s) — no free slots!\n", path);
        errno = ENOMEM;
        return NULL;
    }

    /* Разбор режима открытия */
    int rd = strchr(mode, 'r') != NULL;
    int wr = strchr(mode, 'w') != NULL;
    int ap = strchr(mode, 'a') != NULL;
    int pl = strchr(mode, '+') != NULL;

    BYTE fa;
    if (wr) {
        /* "w" или "w+" — создать/обнулить */
        fa = FA_WRITE | FA_CREATE_ALWAYS;
        if (pl) fa |= FA_READ;
    } else if (ap) {
        /* "a" или "a+" — дописать */
        fa = FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS;
        if (pl) fa |= FA_READ;
    } else {
        /* "r" или "r+" — существующий файл */
        fa = FA_READ;
        if (pl) fa |= FA_WRITE;
        /* FA_OPEN_EXISTING — по умолчанию в FatFS, не нужен явно */
    }

    memset(f, 0, sizeof(*f));
    FRESULT res = f_open(&f->fil, path, fa);
    if (res != FR_OK) {
        DBG_PRINT("fatfs: fopen(%s, \"%s\") err=%d\n", path, mode, res);
        errno = ENOENT;
        return NULL;
    }

    f->used  = 1;
    f->eof   = 0;
    f->error = 0;
    DBG_PRINT("fatfs: fopen(%s, \"%s\") OK slot=%d\n",
              path, mode, (int)(f - _files));
    return fat_to_fp(f);
}

/* ------------------------------------------------------------------ */
/* __wrap_fclose                                                       */
/* ------------------------------------------------------------------ */

int __wrap_fclose(FILE *fp)
{
    if (IS_STDIO(fp)) return 0;
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return EOF;
    f_close(&f->fil);
    f->used = 0;
    DBG_PRINT("fatfs: fclose slot=%d\n", (int)(f - _files));
    return 0;
}

/* ------------------------------------------------------------------ */
/* __wrap_fread                                                        */
/* ------------------------------------------------------------------ */

size_t __wrap_fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return 0;
    if (size == 0 || nmemb == 0) return 0;
    UINT total = (UINT)(size * nmemb);
    UINT bytes_read = 0;
    FRESULT res = f_read(&f->fil, ptr, total, &bytes_read);
    if (res != FR_OK) { f->error = 1; return 0; }
    if (bytes_read < total) f->eof = 1;
    return bytes_read / size;
}

/* ------------------------------------------------------------------ */
/* __wrap_fwrite                                                       */
/* ------------------------------------------------------------------ */

size_t __wrap_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    if (IS_STDIO(fp)) {
        /* Оригинальный fwrite для stdout/stderr */
        extern size_t __real_fwrite(const void*, size_t, size_t, FILE*);
        return __real_fwrite(ptr, size, nmemb, fp);
    }
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return 0;
    if (size == 0 || nmemb == 0) return 0;
    UINT bw = 0;
    FRESULT res = f_write(&f->fil, ptr, (UINT)(size * nmemb), &bw);
    if (res != FR_OK) { f->error = 1; return 0; }
    return bw / size;
}

/* ------------------------------------------------------------------ */
/* __wrap_fseek                                                        */
/* ------------------------------------------------------------------ */

int __wrap_fseek(FILE *fp, long offset, int whence)
{
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return -1;
    FSIZE_t pos;
    switch (whence) {
        case SEEK_SET: pos = (FSIZE_t)offset; break;
        case SEEK_CUR: pos = f_tell(&f->fil) + (FSIZE_t)offset; break;
        case SEEK_END: pos = f_size(&f->fil) + (FSIZE_t)offset; break;
        default: return -1;
    }
    FRESULT res = f_lseek(&f->fil, pos);
    if (res != FR_OK) return -1;
    f->eof = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* __wrap_ftell                                                        */
/* ------------------------------------------------------------------ */

long __wrap_ftell(FILE *fp)
{
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return -1L;
    return (long)f_tell(&f->fil);
}

/* ------------------------------------------------------------------ */
/* __wrap_rewind                                                       */
/* ------------------------------------------------------------------ */

void __wrap_rewind(FILE *fp)
{
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return;
    f_lseek(&f->fil, 0);
    f->eof   = 0;
    f->error = 0;
}

/* ------------------------------------------------------------------ */
/* __wrap_feof / __wrap_ferror / __wrap_clearerr / __wrap_fflush      */
/* ------------------------------------------------------------------ */

int __wrap_feof(FILE *fp)
{
    fatfs_file_t *f = fp_to_fat(fp);
    return f ? f->eof : 0;
}

int __wrap_ferror(FILE *fp)
{
    fatfs_file_t *f = fp_to_fat(fp);
    return f ? f->error : 0;
}

void __wrap_clearerr(FILE *fp)
{
    fatfs_file_t *f = fp_to_fat(fp);
    if (f) { f->eof = 0; f->error = 0; }
}

int __wrap_fflush(FILE *fp)
{
    if (IS_STDIO(fp)) return 0;
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return EOF;
    f_sync(&f->fil);
    return 0;
}

/* ------------------------------------------------------------------ */
/* __wrap_fgetc / __wrap_fputc / __wrap_fputs / __wrap_fgets          */
/* ------------------------------------------------------------------ */

int __wrap_fgetc(FILE *fp)
{
    unsigned char c;
    if (__wrap_fread(&c, 1, 1, fp) != 1) return EOF;
    return (int)c;
}

int __wrap_ungetc(int c, FILE *fp)
{
    /* Простая реализация: откатываем на 1 байт назад */
    (void)c;
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return EOF;
    FSIZE_t pos = f_tell(&f->fil);
    if (pos > 0) f_lseek(&f->fil, pos - 1);
    return c;
}

int __wrap_fputc(int c, FILE *fp)
{
    if (IS_STDIO(fp)) {
        extern int __real_fputc(int, FILE*);
        return __real_fputc(c, fp);
    }
    unsigned char ch = (unsigned char)c;
    if (__wrap_fwrite(&ch, 1, 1, fp) != 1) return EOF;
    return c;
}

int __wrap_fputs(const char *s, FILE *fp)
{
    if (IS_STDIO(fp)) {
        extern int __real_fputs(const char*, FILE*);
        return __real_fputs(s, fp);
    }
    size_t len = strlen(s);
    return (__wrap_fwrite(s, 1, len, fp) == len) ? (int)len : EOF;
}

char *__wrap_fgets(char *s, int n, FILE *fp)
{
    fatfs_file_t *f = fp_to_fat(fp);
    if (!f) return NULL;
    if (f_gets(s, n, &f->fil) == NULL) { f->eof = 1; return NULL; }
    return s;
}

/* ------------------------------------------------------------------ */
/* __wrap_fprintf                                                      */
/* ------------------------------------------------------------------ */

int __wrap_fprintf(FILE *fp, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int r;
    if (IS_STDIO(fp)) {
        r = vfprintf(fp, fmt, va);
    } else {
        char buf[512];
        int len = vsnprintf(buf, sizeof(buf), fmt, va);
        if (len > 0) {
            if (len > (int)sizeof(buf)) len = sizeof(buf);
            r = (int)__wrap_fwrite(buf, 1, len, fp);
        } else r = 0;
    }
    va_end(va);
    return r;
}

/* ------------------------------------------------------------------ */
/* __wrap_vfprintf                                                     */
/* ------------------------------------------------------------------ */

int __wrap_vfprintf(FILE *fp, const char *fmt, va_list va)
{
    if (IS_STDIO(fp)) {
        extern int __real_vfprintf(FILE*, const char*, va_list);
        return __real_vfprintf(fp, fmt, va);
    }
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf), fmt, va);
    if (len <= 0) return 0;
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    return (int)__wrap_fwrite(buf, 1, len, fp);
}
