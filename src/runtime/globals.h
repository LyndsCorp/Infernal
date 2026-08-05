/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: runtime/globals.h
*/

#ifndef RUNTIME_GLOBALS_H
#define RUNTIME_GLOBALS_H

#include "core/types.h"   // ya contiene FuncObject con el campo code
#include "runtime/scope.h"
#include "vm/vm.h"        // para Chunk (pero solo se usa el forward declaration en types.h)

/* ─── Control flow constants ────────────────────────────── */
#define CF_NONE        0
#define CF_RETURN      1
#define CF_BREAK       2
#define CF_CONTINUE    3
#define CF_REPEAT_LINE 4

/* ─── Global scopes ──────────────────────────────────────── */
extern Scope *global_scope;
extern Scope *super_global_scope;
extern Scope *current_scope;

/* ─── Function table ────────────────────────────────────── */
typedef struct FuncEntry {
    char *name;
    FuncObject *obj;
    struct FuncEntry *next;
} FuncEntry;

extern FuncEntry *func_table;

void func_register(const char *name, ASTNode *def);
void func_register_builtin(const char *name, BuiltinFunc fn);
FuncObject *func_lookup(const char *name);

/* ─── Import prefix y límite de iteraciones ─────────────── */
extern char *current_import_prefix;
extern int max_loop_iterations;

/* ─── Error y control de flujo ───────────────────────────── */
extern jmp_buf exception_env;
extern int exception_raised;
extern char exception_msg[512];
extern char **source_lines;
extern int source_line_count;
extern int control_flow;
extern Value return_value;

/* ─── Argumentos del script ──────────────────────────────── */
extern int script_argc;
extern char **script_argv;

/* ─── Repeat line target ─────────────────────────────────── */
extern int repeat_line_target;

/* ─── Directorio del script y archivo actual ─────────────── */
extern char *script_dir;
extern char *current_source_file;

/* ─── Shell configurado para ejecutar comandos del sistema ── */
extern char *infernal_shell;
void load_infernal_config(void);
void show_shell_info(void);

extern int current_eval_line;
extern int flags_arg_index;   /* <-- NUEVO: índice global para argumentos en modo 1 */

#endif
