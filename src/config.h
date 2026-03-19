/**
 * src/config.h — для RP2350 (заменяет autoconf-генерируемый файл).
 *
 * Критичное отличие от оригинала: HAVE_STDINT_H и HAVE_INTTYPES_H
 * определены как 1. В оригинале они были #undef, что на ARM newlib
 * (Pico SDK) ломало резолюцию прототипов из <string.h> и <stdint.h>
 * через механизм feature-test macros.
 */

#ifndef PCE_CONFIG_H
#define PCE_CONFIG_H 1

/* Доступно в newlib (Pico SDK) */
#define HAVE_STDINT_H    1
#define HAVE_INTTYPES_H  1
#define HAVE_LIMITS_H    1

/* POSIX — нет на RP2350 */
#undef HAVE_FCNTL_H
#undef HAVE_TERMIOS_H
#undef HAVE_UNISTD_H
#undef HAVE_LINUX_IF_TUN_H
#undef HAVE_LINUX_TCP_H
#undef HAVE_SYS_IOCTL_H
#undef HAVE_SYS_POLL_H
#undef HAVE_SYS_SOCKET_H
#undef HAVE_SYS_SOUNDCARD_H
#undef HAVE_SYS_TIME_H
#undef HAVE_SYS_TYPES_H

/* POSIX функции — заменены в rp2350_stubs.c */
#undef HAVE_FSEEKO
#undef HAVE_FTRUNCATE
#undef HAVE_USLEEP
#undef HAVE_NANOSLEEP
#undef HAVE_SLEEP
#undef HAVE_GETTIMEOFDAY

/* Платформенные драйверы */
#undef PCE_ENABLE_X11
#undef PCE_ENABLE_SDL
#undef PCE_ENABLE_READLINE
#undef PCE_ENABLE_TUN
#undef PCE_ENABLE_CHAR_POSIX
#undef PCE_ENABLE_CHAR_PPP
#undef PCE_ENABLE_CHAR_PTY
#undef PCE_ENABLE_CHAR_SLIP
#undef PCE_ENABLE_CHAR_TCP
#undef PCE_ENABLE_CHAR_TIOS
#undef PCE_ENABLE_SOUND_OSS

#undef PCE_HOST_LINUX
#undef PCE_HOST_WINDOWS
#undef PCE_HOST_IA32
#undef PCE_HOST_PPC
#undef PCE_HOST_SPARC

#define PCE_DIR_SEP '/'

#ifndef PCE_VERSION_STR
#define PCE_VERSION_STR "0.0.0"
#endif

#endif /* PCE_CONFIG_H */
