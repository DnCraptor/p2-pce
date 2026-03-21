/**
 * rp2350_mac.h — интерфейс RP2350 ↔ PCE Mac Plus
 */

#ifndef RP2350_MAC_H
#define RP2350_MAC_H

#include <stdbool.h>

/* Forward declaration — macplus.h требует быть в src/arch/macplus/ context,
 * поэтому включаем его только в rp2350_mac.c. Вызывающий код (main.c)
 * тип macplus_t знать не обязан — он работает только через эти функции. */
struct macplus_s;
typedef struct macplus_s macplus_t;

/**
 * Инициализировать эмулятор: загрузить конфиг, ROM, диск, сбросить CPU.
 * Вызывать после init_hardware() и монтирования SD.
 * Возвращает сообщение об ошибке (нет ROM, нет конфига и т.д.), или NULL.
 */
const char*  rp2350_mac_init(void);

/**
 * Один шаг эмуляции (одна инструкция 68000 + все клоки периферии).
 * Вызывать в плотном цикле из Core 0.
 */
void rp2350_mac_step(void);

/** Текущее состояние эмулятора */
bool       rp2350_mac_is_running(void);
macplus_t *rp2350_mac_get_sim(void);

/**
 * Передать нажатие/отпускание клавиши.
 * hid_keycode — код из ps2kbd / usbhid (формат HID Usage).
 */
void rp2350_mac_send_key(int is_down, int hid_keycode);

/**
 * Передать движение мыши.
 * dx, dy — относительные смещения; buttons — маска кнопок.
 */
void rp2350_mac_send_mouse(int dx, int dy, unsigned buttons);

#endif /* RP2350_MAC_H */
