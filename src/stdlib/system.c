/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: stdlib/system.c
*/

#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "core/value.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "runtime/command.h"
#include "vm/vm.h"

static Value builtin_exit(int argc, Value *args) {
    int code = 0;
    if (argc > 0) {
        Value v = args[0];
        code = (v.type == VAL_INT) ? v.data.ival : 0;
    }
    exit(code);
}

static Value builtin_setlooplimit(int argc, Value *args) {
    if (argc < 1) error(0, "setlooplimit requiere un argumento");
    Value v = args[0];
    if (v.type == VAL_INT) max_loop_iterations = v.data.ival;
    else error(0, "setlooplimit espera un entero");
    return val_make_null();
}

static Value builtin_getlooplimit(int argc, Value *args) {
    (void)argc; (void)args;
    return val_int(max_loop_iterations);
}

static Value builtin_here(int argc, Value *args) {
    (void)argc;
    (void)args;
    const char *dir = script_dir;
    if (!dir || dir[0] == '\0') dir = ".";
    return val_string(dir);
}

/* ─── NUEVA FUNCIÓN exited ────────────────────────────────── */
static Value builtin_exited(int argc, Value *args) {
    if (argc != 1) error(0, "exited requiere un argumento (el comando)");
    if (args[0].type != VAL_STRING) error(0, "exited espera un string");
    const char *cmd = args[0].data.sval;
    int code = run_command_get_exit_code(cmd);
    return val_int(code);
}

void register_system_builtins(void) {
    func_register_builtin("exit", builtin_exit);
    func_register_builtin("setlooplimit", builtin_setlooplimit);
    func_register_builtin("getlooplimit", builtin_getlooplimit);
    func_register_builtin("here", builtin_here);
    func_register_builtin("exited", builtin_exited);

    vm_register_builtin("exit", builtin_exit);
    vm_register_builtin("setlooplimit", builtin_setlooplimit);
    vm_register_builtin("getlooplimit", builtin_getlooplimit);
    vm_register_builtin("here", builtin_here);
    vm_register_builtin("exited", builtin_exited);
}
