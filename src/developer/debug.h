/*
 * Infernal: el lenguaje de programación.
 * Copyright (C) 2026, Lynds Corp., David Baña Szymaniak, GPL v3+ License.
 * Código fuente de Infernal: developer/debug.h
 *
 * Módulo de depuración. Activar con -DDEBUG en tiempo de compilación.
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
#define DEBUG_VM(fmt, ...)     DEBUG_LOG("VM", DEBUG_COLOR_MAGENTA, fmt, ##__VA_ARGS__)
#define DEBUG_COMPILER(fmt, ...) DEBUG_LOG("COMPILER", DEBUG_COLOR_BLUE, fmt, ##__VA_ARGS__)

/* --- Macro para trazar ejecución de bytecode ------------------ */
#define DEBUG_TRACE_OP(op, desc) \
DEBUG_OP("%s (op=%d, operand=%d, operand2=%d)", desc, ip->op, ip->operand, ip->operand2)

/* --- Macro para mostrar estado de la pila --------------------- */
#define DEBUG_STACK() \
do { \
    if (sp > stack) { \
        DEBUG_VM("Stack: [%d elements]", (int)(sp - stack)); \
        for (Value *p = stack; p < sp; p++) { \
            switch (p->type) { \
                case VAL_INT:    DEBUG_VM("  [%ld] int: %d", (long)(p - stack), p->data.ival); break; \
                case VAL_FLOAT:  DEBUG_VM("  [%ld] float: %g", (long)(p - stack), p->data.fval); break; \
                case VAL_BOOL:   DEBUG_VM("  [%ld] bool: %s", (long)(p - stack), p->data.bval ? "true" : "false"); break; \
                case VAL_STRING: DEBUG_VM("  [%ld] string: \"%s\"", (long)(p - stack), p->data.sval); break; \
                case VAL_LIST:   DEBUG_VM("  [%ld] list: [%d elements]", (long)(p - stack), p->data.list.count); break; \
                case VAL_NULL:   DEBUG_VM("  [%ld] null", (long)(p - stack)); break; \
                default:         DEBUG_VM("  [%ld] unknown type: %d", (long)(p - stack), p->type); break; \
            } \
        } \
    } else { \
        DEBUG_VM("Stack: empty"); \
    } \
} while(0)

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
#define DEBUG_VM(fmt, ...)
#define DEBUG_COMPILER(fmt, ...)
#define DEBUG_TRACE_OP(op, desc)
#define DEBUG_STACK()
#define DEBUG_VAR(name, value)
#define DEBUG_ENTER(fn)
#define DEBUG_LEAVE(fn)

#endif /* DEBUG */

#endif /* DEVELOPER_DEBUG_H */
