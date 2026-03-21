/**
 * Debug output macros
 *
 * Use DBG_PRINT for informational debug messages that should only
 * appear when DEBUG_ENABLED=1 is set at compile time.
 *
 * Use printf directly for critical errors that should always be shown.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

#ifdef DEBUG_ENABLED
void debug_log(const char *fmt, ...);
#define DBG_PRINT(...) debug_log(__VA_ARGS__)
#else
#define DBG_PRINT(...) ((void)0)
#endif

#endif /* DEBUG_H */
