/**
 * rp2350_mac.c — интеграция PCE Mac Plus в платформу RP2350.
 *
 * Headless-режим: CPU работает, VBI-прерывания генерируются,
 * диск/ROM загружается с SD — видео пока не выводится.
 *
 * Включается в главный цикл src/main.c вместо закомментированных
 * pc_step()/mac_run() вызовов.
 *
 * Архитектура памяти (текущая, headless):
 *   0x000000 .. RAM_end   — ОЗУ Mac (malloc → PSRAM через pcmalloc)
 *   0x400000 .. 0x40FFFF  — ROM (malloc → PSRAM)
 *   vbuf = RAM_end-0x5900 — видеобуфер живёт в PSRAM
 *
 * Архитектура памяти (целевая, с видео):
 *   vbuf-область дополнительно отзеркалена в SRAM через DMA/прямую
 *   запись — реализовывать отдельно поверх этого файла.
 */

#include "rp2350_mac.h"
#include "rp2350_ini.h"

#include "debug.h"
#include "board_config.h"
#include "config_save.h"

/* PCE arch — macplus.h использует относительные includes ("adb.h" и т.д.),
 * которые резолвятся корректно, т.к. src/arch/macplus прописан в include_dirs */
#include <arch/macplus/main.h>
#include <arch/macplus/macplus.h>
#include <arch/macplus/cmd_68k.h>
#include <arch/macplus/keyboard.h>

/* PCE lib — заголовки нужны для прототипов, реализации в rp2350_stubs.c */
#include <lib/sysdep.h>

/* Pico SDK */
#include "pico/stdlib.h"
#include "hardware/timer.h"

/* FatFS */
#include "ff.h"

/* ------------------------------------------------------------------ */
/* Глобальные переменные, ожидаемые arch/macplus/main.h               */
/* ------------------------------------------------------------------ */

#include <lib/monitor.h>

macplus_t  *par_sim        = NULL;
monitor_t   par_mon;          /* нужен msg.c; headless — не используется */
unsigned    par_sig_int    = 0;
const char *par_terminal   = NULL;   /* NULL → trm==NULL, headless */

unsigned    par_disk_delay_valid = 0;
unsigned    par_disk_delay[SONY_DRIVES];

/* ------------------------------------------------------------------ */
/* Внутреннее состояние                                               */
/* ------------------------------------------------------------------ */

static bool mac_running = false;

/* Счётчик итераций для периодического poll_keyboard */
static uint32_t mac_poll_counter = 0;
#define MAC_POLL_INTERVAL  2048   /* вызовов mac_clock до poll_keyboard */

/* ------------------------------------------------------------------ */
/* Вспомогательное: диагностика из mac_log_deb                        */
/* ------------------------------------------------------------------ */

void mac_log_deb(const char *msg, ...)
{
    va_list va;
    va_start(va, msg);
    if (par_sim && par_sim->cpu) {
        unsigned long pc = e68_get_pc(par_sim->cpu);
        DBG_PRINT("[%06lX] ", pc & 0xffffff);
    }
    /* dbg_vprint из rp2350_stubs.c форматирует в буфер → DBG_PRINT */
    char buf[256];
    vsnprintf(buf, sizeof(buf), msg, va);
    va_end(va);
    DBG_PRINT("%s", buf);
}

/* ------------------------------------------------------------------ */
/* Инициализация эмулятора                                            */
/* ------------------------------------------------------------------ */
const char* rp2350_mac_init(void)
{
    DBG_PRINT("[mac] Initializing Mac Plus emulator...\n");

    /* Загрузить INI с SD */
    const char* err;
    ini_sct_t *cfg = rp2350_load_ini("pce/config.ini", &err);
    if (!cfg) {
        return err;
    }

    /* Найти секцию [macplus] или взять root */
    ini_sct_t *mac_sct = ini_next_sct(cfg, NULL, "macplus");
    if (!mac_sct) {
        mac_sct = cfg;   /* секции прямо в корне */
    }

    /* Создать экземпляр эмулятора */
    par_sim = mac_new(mac_sct);
    ini_sct_del(cfg);

    if (!par_sim) {
        return "[mac] ERROR: mac_new() failed";
    }

    /* Сброс CPU и периферии */
    mac_reset(par_sim);

    mac_running = true;

    DBG_PRINT("[mac] Mac Plus ready, PC=0x%08lX\n",
              (unsigned long)e68_get_pc(par_sim->cpu));
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Один шаг эмуляции — вызывать из главного цикла Core 0              */
/* ------------------------------------------------------------------ */
/*
 * Стратегия: вызываем mac_clock(sim, 0) в плотном цикле.
 *   n=0 → mac_clock сам читает cpu->delay (число тактов последней инструкции).
 *
 * mac_realtime_sync() внутри mac_clock вызывает pce_usleep() если эмуляция
 * обгоняет реальное время. На RP2350 pce_usleep → sleep_us → не блокирует Core 1.
 *
 * Для headless главный цикл выглядит так:
 *
 *   while (true) {
 *       rp2350_mac_step();
 *       // периодически: poll_keyboard(), diskui_animate() и т.д.
 *   }
 */
void rp2350_mac_step(void)
{
    if (!mac_running || !par_sim) return;

    if (par_sim->brk) {
        /* Эмулятор остановился (PANic, halt и т.д.) */
        DBG_PRINT("[mac] brk=%u, PC=0x%08lX\n",
                  par_sim->brk,
                  (unsigned long)e68_get_pc(par_sim->cpu));
        mac_running = false;
        return;
    }

    mac_clock(par_sim, 0);
}

/* ------------------------------------------------------------------ */
/* Проверка состояния                                                  */
/* ------------------------------------------------------------------ */

bool rp2350_mac_is_running(void) { return mac_running; }

macplus_t *rp2350_mac_get_sim(void) { return par_sim; }

const unsigned char* __not_in_flash_func(rp2350_mac_get_vbuf)(void) {
    if (!par_sim || !par_sim->video || !par_sim->video->trm) return NULL;
    return par_sim->video->trm->buf;
}

/* ------------------------------------------------------------------ */
/* Linux evdev keycode → pce_key_t                                    */
/* ------------------------------------------------------------------ */
/* ps2kbd_wrapper отдаёт Linux input keycodes (evdev).
 * PCE не имеет встроенного маппера из evdev, поэтому таблица здесь.
 * Значения evdev: /usr/include/linux/input-event-codes.h */
static pce_key_t evdev_to_pce(int ev)
{
    switch (ev) {
    case   1: return PCE_KEY_ESC;
    case   2: return PCE_KEY_1;
    case   3: return PCE_KEY_2;
    case   4: return PCE_KEY_3;
    case   5: return PCE_KEY_4;
    case   6: return PCE_KEY_5;
    case   7: return PCE_KEY_6;
    case   8: return PCE_KEY_7;
    case   9: return PCE_KEY_8;
    case  10: return PCE_KEY_9;
    case  11: return PCE_KEY_0;
    case  12: return PCE_KEY_MINUS;
    case  13: return PCE_KEY_EQUAL;
    case  14: return PCE_KEY_BACKSPACE;
    case  15: return PCE_KEY_TAB;
    case  16: return PCE_KEY_Q;
    case  17: return PCE_KEY_W;
    case  18: return PCE_KEY_E;
    case  19: return PCE_KEY_R;
    case  20: return PCE_KEY_T;
    case  21: return PCE_KEY_Y;
    case  22: return PCE_KEY_U;
    case  23: return PCE_KEY_I;
    case  24: return PCE_KEY_O;
    case  25: return PCE_KEY_P;
    case  26: return PCE_KEY_LBRACKET;
    case  27: return PCE_KEY_RBRACKET;
    case  28: return PCE_KEY_RETURN;
    case  29: return PCE_KEY_LCTRL;
    case  30: return PCE_KEY_A;
    case  31: return PCE_KEY_S;
    case  32: return PCE_KEY_D;
    case  33: return PCE_KEY_F;
    case  34: return PCE_KEY_G;
    case  35: return PCE_KEY_H;
    case  36: return PCE_KEY_J;
    case  37: return PCE_KEY_K;
    case  38: return PCE_KEY_L;
    case  39: return PCE_KEY_SEMICOLON;
    case  40: return PCE_KEY_QUOTE;
    case  41: return PCE_KEY_BACKQUOTE;
    case  42: return PCE_KEY_LSHIFT;
    case  43: return PCE_KEY_BACKSLASH;
    case  44: return PCE_KEY_Z;
    case  45: return PCE_KEY_X;
    case  46: return PCE_KEY_C;
    case  47: return PCE_KEY_V;
    case  48: return PCE_KEY_B;
    case  49: return PCE_KEY_N;
    case  50: return PCE_KEY_M;
    case  51: return PCE_KEY_COMMA;
    case  52: return PCE_KEY_PERIOD;
    case  53: return PCE_KEY_SLASH;
    case  54: return PCE_KEY_RSHIFT;
    case  55: return PCE_KEY_KP_STAR;
    case  56: return PCE_KEY_LALT;
    case  57: return PCE_KEY_SPACE;
    case  58: return PCE_KEY_CAPSLOCK;
    case  59: return PCE_KEY_F1;
    case  60: return PCE_KEY_F2;
    case  61: return PCE_KEY_F3;
    case  62: return PCE_KEY_F4;
    case  63: return PCE_KEY_F5;
    case  64: return PCE_KEY_F6;
    case  65: return PCE_KEY_F7;
    case  66: return PCE_KEY_F8;
    case  67: return PCE_KEY_F9;
    case  68: return PCE_KEY_F10;
    case  69: return PCE_KEY_NUMLOCK;
    case  70: return PCE_KEY_SCRLK;
    case  71: return PCE_KEY_KP_7;
    case  72: return PCE_KEY_KP_8;
    case  73: return PCE_KEY_KP_9;
    case  74: return PCE_KEY_KP_MINUS;
    case  75: return PCE_KEY_KP_4;
    case  76: return PCE_KEY_KP_5;
    case  77: return PCE_KEY_KP_6;
    case  78: return PCE_KEY_KP_PLUS;
    case  79: return PCE_KEY_KP_1;
    case  80: return PCE_KEY_KP_2;
    case  81: return PCE_KEY_KP_3;
    case  82: return PCE_KEY_KP_0;
    case  83: return PCE_KEY_KP_PERIOD;
    case  86: return PCE_KEY_LESS;
    case  87: return PCE_KEY_F11;
    case  88: return PCE_KEY_F12;
    case  96: return PCE_KEY_KP_ENTER;
    case  97: return PCE_KEY_RCTRL;
    case  98: return PCE_KEY_KP_SLASH;
    case  99: return PCE_KEY_PRTSCN;
    case 100: return PCE_KEY_RALT;
    case 102: return PCE_KEY_HOME;
    case 103: return PCE_KEY_UP;
    case 104: return PCE_KEY_PAGEUP;
    case 105: return PCE_KEY_LEFT;
    case 106: return PCE_KEY_RIGHT;
    case 107: return PCE_KEY_END;
    case 108: return PCE_KEY_DOWN;
    case 109: return PCE_KEY_PAGEDN;
    case 110: return PCE_KEY_INS;
    case 111: return PCE_KEY_DEL;
    case 119: return PCE_KEY_PAUSE;
    case 125: return PCE_KEY_LSUPER;
    case 126: return PCE_KEY_RSUPER;
    case 127: return PCE_KEY_MENU;
    default:  return PCE_KEY_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* Ввод клавиатуры → Mac kbd                                          */
/* ------------------------------------------------------------------ */
void rp2350_mac_send_key(int is_down, int evdev_keycode)
{
    if (!par_sim) return;

    pce_key_t pkey = evdev_to_pce(evdev_keycode);
    if (pkey == PCE_KEY_NONE) return;

    unsigned event = is_down ? PCE_KEY_EVENT_DOWN : PCE_KEY_EVENT_UP;

    if (par_sim->kbd) {
        mac_kbd_set_key(par_sim->kbd, event, pkey);
    }
}

/* ------------------------------------------------------------------ */
/* Мышь → Mac                                                         */
/* ------------------------------------------------------------------ */

void rp2350_mac_send_mouse(int dx, int dy, unsigned buttons)
{
    if (!par_sim) return;
    /* mac_set_mouse — статическая в macplus.c, экспортируем через wrapper */
    par_sim->mouse_delta_x += dx;
    par_sim->mouse_delta_y += dy;
    par_sim->mouse_button   = buttons;
}
