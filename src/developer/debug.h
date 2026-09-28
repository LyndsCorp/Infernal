/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: developer/debug.c
 *
 * Módulo de depuración. Activar con -DDEBUG en tiempo de compilación. make debug lo hace automaticamente.
*/

#ifndef DEVELOPER_DEBUG_H
#define DEVELOPER_DEBUG_H

#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#ifdef DEBUG

/* --- Colores ANSI para logs ----------------------------------- */
#define DEBUG_COLOR_RESET   "\033[0m"
#define DEBUG_COLOR_RED     "\033[31m"
#define DEBUG_COLOR_GREEN   "\033[32m"
#define DEBUG_COLOR_YELLOW  "\033[33m"
#define DEBUG_COLOR_BLUE    "\033[34m"
#define DEBUG_COLOR_MAGENTA "\033[35m"
#define DEBUG_COLOR_CYAN    "\033[36m"

/* --- Macro de depuración principal ---------------------------- */
#define DEBUG_LOG(level, color, fmt, ...) \
do { \
    struct timeval tv; \
    gettimeofday(&tv, NULL); \
    time_t now = tv.tv_sec; \
    struct tm *tm_info = localtime(&now); \
    char timebuf[64]; \
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info); \
    fprintf(stderr, "%s[%s.%03ld] %s" color "[%s]" DEBUG_COLOR_RESET " " fmt "\n", \
    DEBUG_COLOR_RESET, timebuf, tv.tv_usec / 1000, \
    DEBUG_COLOR_RESET, level, ##__VA_ARGS__); \
    fflush(stderr); \
} while(0)

/* --- Niveles de log ------------------------------------------- */
#define DEBUG_INFO(fmt, ...)   DEBUG_LOG("INFO", DEBUG_COLOR_GREEN, fmt, ##__VA_ARGS__)
#define DEBUG_WARN(fmt, ...)   DEBUG_LOG("WARN", DEBUG_COLOR_YELLOW, fmt, ##__VA_ARGS__)
#define DEBUG_ERROR(fmt, ...)  DEBUG_LOG("ERROR", DEBUG_COLOR_RED, fmt, ##__VA_ARGS__)
#define DEBUG_OP(fmt, ...)     DEBUG_LOG("OP", DEBUG_COLOR_CYAN, fmt, ##__VA_ARGS__)

/* --- Macro para mostrar valor de una variable ----------------- */
#define DEBUG_VAR(name, value) \
do { \
    switch ((value).type) { \
        case VAL_INT:    DEBUG_INFO("Variable '%s' = %d (int)", name, (value).data.ival); break; \
        case VAL_FLOAT:  DEBUG_INFO("Variable '%s' = %g (float)", name, (value).data.fval); break; \
        case VAL_BOOL:   DEBUG_INFO("Variable '%s' = %s (bool)", name, (value).data.bval ? "true" : "false"); break; \
        case VAL_STRING: DEBUG_INFO("Variable '%s' = \"%s\" (string)", name, (value).data.sval); break; \
        case VAL_LIST:   DEBUG_INFO("Variable '%s' = [%d elements] (list)", name, (value).data.list.count); break; \
        case VAL_NULL:   DEBUG_INFO("Variable '%s' = null", name); break; \
        default:         DEBUG_INFO("Variable '%s' = unknown type %d", name, (value).type); break; \
    } \
} while(0)

/* --- Macro para marcar entrada/salida de funciones ------------ */
#define DEBUG_ENTER(fn) DEBUG_INFO("→ Entering %s()", fn)
#define DEBUG_LEAVE(fn) DEBUG_INFO("← Leaving %s()", fn)

#else /* !DEBUG */

/* --- Modo producción: todas las macros se expanden a nada ---- */
#define DEBUG_INFO(fmt, ...)
#define DEBUG_WARN(fmt, ...)
#define DEBUG_ERROR(fmt, ...)
#define DEBUG_OP(fmt, ...)
#define DEBUG_VAR(name, value)
#define DEBUG_ENTER(fn)
#define DEBUG_LEAVE(fn)

#endif /* DEBUG */

#endif /* DEVELOPER_DEBUG_H */
