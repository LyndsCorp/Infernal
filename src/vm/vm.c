/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: vm/vm.c
*/

#include "vm.h"
#include "core/value.h"
#include "runtime/command.h"
#include "runtime/error.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/evaluator/evaluator.h"
#include "core/ast.h"
#include "lexer/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

extern int current_eval_line;

#define STACK_MAX 4096
Value stack[STACK_MAX];
static Value *sp = stack;

static inline void push(Value v) { *sp++ = v; }
static inline Value pop(void)  { return *--sp; }
static inline Value peek(int dist) { return *(sp - 1 - dist); }

/* --- Globales de la VM ------------------------------------- */
typedef struct {
    char *name;
    int scope_type;
} GlobalEntry;

#define GLOBAL_SCRIPT 0
#define GLOBAL_SUPER  1

GlobalEntry vm_global_entries[MAX_GLOBALS];
int vm_global_count = 0;
char *vm_global_names[MAX_GLOBALS];
Value vm_globals[MAX_GLOBALS];
int vm_global_types[MAX_GLOBALS] = {0};

VmBuiltin vm_builtins[256];
int vm_builtin_count = 0;

static struct { const char *name; VmBuiltin func; } builtin_names[256];
static int builtin_names_count = 0;

typedef struct {
    char *name;
    Chunk *code;
} UserFunction;
static UserFunction user_functions[256];
static int user_function_count = 0;

/* --- Registro de globales ------------------------------------- */
int vm_register_global(const char *name, int scope_type, int vtype) {
    if (vm_global_count >= MAX_GLOBALS) return -1;
    vm_global_entries[vm_global_count].name = strdup(name);
    vm_global_entries[vm_global_count].scope_type = scope_type;
    vm_globals[vm_global_count] = val_make_null();
    vm_global_types[vm_global_count] = vtype;
    vm_global_names[vm_global_count] = vm_global_entries[vm_global_count].name;
    return vm_global_count++;
}

int vm_find_global_index(const char *name) {
    for (int i = 0; i < vm_global_count; i++) {
        if (vm_global_entries[i].name && strcmp(vm_global_entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* --- Registro de builtins ------------------------------------- */
int vm_register_builtin(const char *name, VmBuiltin func) {
    if (vm_builtin_count >= 256) return -1;
    builtin_names[builtin_names_count].name = name;
    builtin_names[builtin_names_count].func = func;
    builtin_names_count++;
    vm_builtins[vm_builtin_count] = func;
    return vm_builtin_count++;
}

int vm_find_builtin_index(const char *name) {
    for (int i = 0; i < builtin_names_count; i++)
        if (strcmp(builtin_names[i].name, name) == 0)
            return i;
    return -1;
}

/* --- Registro de funciones de usuario --------------------- */
int vm_register_user_function(const char *name, Chunk *code) {
    if (user_function_count >= 256) return -1;
    user_functions[user_function_count].name = strdup(name);
    user_functions[user_function_count].code = code;
    return user_function_count++;
}

Chunk *vm_get_user_function(int index) {
    if (index < 0 || index >= user_function_count) return NULL;
    return user_functions[index].code;
}

static int call_builtin(int index, int arg_count) {
    if (index >= vm_builtin_count) error(current_eval_line, "Índice de builtin inválido");
    Value *args = sp - arg_count;
    Value ret = vm_builtins[index](arg_count, args);
    sp -= arg_count;
    push(ret);
    return 1;
}

static char *expand_command_vm(Chunk *chunk, Value *locals, const char *cmd) {
    return expand_command_with_locals(cmd, chunk->local_names, locals, chunk->local_count);
}

/* --- Función de conversión de tipos para la VM --------------- */
static Value vm_convert_value(Value v, int target_tok_type) {
    if (target_tok_type == 0) return v;

    if (v.type == VAL_STRING) {
        const char *s = v.data.sval;
        if (target_tok_type == TOK_INT) {
            char *end;
            long n = strtol(s, &end, 10);
            if (*end == '\0' && end != s) {
                free(v.data.sval);
                v.type = VAL_INT;
                v.data.ival = (int)n;
                return v;
            }
        } else if (target_tok_type == TOK_FLOAT) {
            char *normalized = strdup(s);
            for (char *p = normalized; *p; p++) if (*p == ',') *p = '.';
            char *end;
            double f = strtod(normalized, &end);
            if (*end == '\0' && end != normalized) {
                free(v.data.sval);
                v.type = VAL_FLOAT;
                v.data.fval = f;
                free(normalized);
                return v;
            }
            free(normalized);
        } else if (target_tok_type == TOK_BOOL) {
            if (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0) {
                free(v.data.sval);
                v.type = VAL_BOOL;
                v.data.bval = true;
                return v;
            }
            if (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0) {
                free(v.data.sval);
                v.type = VAL_BOOL;
                v.data.bval = false;
                return v;
            }
        }
        return v;
    }

    if (v.type == VAL_INT && target_tok_type == TOK_FLOAT) {
        v.type = VAL_FLOAT;
        v.data.fval = (double)v.data.ival;
        return v;
    }
    if (v.type == VAL_FLOAT && target_tok_type == TOK_INT) {
        v.type = VAL_INT;
        v.data.ival = (int)v.data.fval;
        return v;
    }
    return v;
}

#if defined(__GNUC__) || defined(__clang__)
#define USE_COMPUTED_GOTO 1
#endif

extern Scope *global_scope;
extern Scope *super_global_scope;

/* --- Ejecución de bytecode ------------------------------------- */
Value vm_run(Chunk *chunk) {
    if (!chunk || chunk->code_count == 0) return val_make_null();

    Value locals[256] = {{0}};
    Instruction *ip = chunk->code;

    #ifdef USE_COMPUTED_GOTO
    static void *dispatch_table[] = {
        &&OP_NOP, &&OP_PUSH_INT, &&OP_PUSH_FLOAT, &&OP_PUSH_STRING, &&OP_PUSH_BOOL, &&OP_PUSH_NULL,
        &&OP_LOAD_VAR, &&OP_STORE_VAR,
        &&OP_LOAD_GLOBAL, &&OP_STORE_GLOBAL,
        &&OP_ADD, &&OP_SUB, &&OP_MUL, &&OP_DIV, &&OP_MOD,
        &&OP_NEG,
        &&OP_EQ, &&OP_NEQ, &&OP_LT, &&OP_GT, &&OP_LE, &&OP_GE,
        &&OP_AND, &&OP_OR, &&OP_NOT,
        &&OP_CALL_BUILTIN, &&OP_CALL_USER,
        &&OP_RETURN,
        &&OP_JUMP_IF_FALSE, &&OP_JUMP,
        &&OP_DUP, &&OP_POP,
        &&OP_NEW_LIST, &&OP_LIST_APPEND,
        &&OP_INDEX, &&OP_INDEX_ASSIGN,
        &&OP_EMBEDDED_CMD, &&OP_SHELL_CMD, &&OP_FLAGS,
        &&OP_CMD_ASSIGN, &&OP_INTERPRET_NODE,
        &&OP_LIST_INSERT,
        &&OP_NEW_MAP, &&OP_MAP_SET
    };
    #define DISPATCH() do { current_eval_line = ip->line; goto *dispatch_table[ip->op]; } while(0)
    goto *dispatch_table[ip->op];
    #else
    for (;;) {
        current_eval_line = ip->line;
        switch (ip->op) {
            #endif

            OP_NOP: ip++; DISPATCH();

            OP_PUSH_INT:    push(chunk->constants[ip->operand]); ip++; DISPATCH();
            OP_PUSH_FLOAT:  push(chunk->constants[ip->operand]); ip++; DISPATCH();
            OP_PUSH_STRING: push(chunk->constants[ip->operand]); ip++; DISPATCH();
            OP_PUSH_BOOL:   push(chunk->constants[ip->operand]); ip++; DISPATCH();
            OP_PUSH_NULL:   push(val_make_null()); ip++; DISPATCH();

            OP_LOAD_VAR: {
                Value v = locals[ip->operand];
                if (v.type == VAL_NULL) {
                    error(current_eval_line, "Variable local no definida");
                }
                if (v.type == VAL_STRING) {
                    v = val_string(v.data.sval);
                }
                push(v);
                ip++;
                DISPATCH();
            }
            OP_STORE_VAR: {
                Value val = pop();
                int slot = ip->operand;
                if (slot < chunk->local_count && chunk->local_types[slot] != 0) {
                    int target_type = chunk->local_types[slot];
                    if (target_type == TOK_STRING && val.type == VAL_LIST) {
                        if (!try_convert_value(&val, TOK_STRING)) {
                            error(current_eval_line, "No se pudo convertir lista a string en asignación a local '%s'",
                                  chunk->local_names[slot] ? chunk->local_names[slot] : "?");
                        }
                    } else {
                        val = vm_convert_value(val, target_type);
                    }
                }
                locals[slot] = val;
                if (slot < chunk->local_count && chunk->local_names[slot]) {
                    const char *name = chunk->local_names[slot];
                    VarEntry *e = scope_find(current_scope, name);
                    if (e) {
                        scope_assign(current_scope, name, val, 0);
                    } else {
                        int vtype = valtype_to_tokentype(val.type);
                        scope_define(current_scope, name, vtype, val);
                    }
                }
                ip++;
                DISPATCH();
            }

            OP_LOAD_GLOBAL: {
                if (ip->operand < vm_global_count) {
                    Value v = vm_globals[ip->operand];
                    if (v.type == VAL_NULL) {
                        error(current_eval_line, "Variable global no definida: %s", vm_global_names[ip->operand]);
                    }
                    push(copy_value_secure(v));
                } else {
                    error(current_eval_line, "Acceso a global inválido");
                }
                ip++;
                DISPATCH();
            }

            OP_STORE_GLOBAL: {
                Value val = pop();
                int idx = ip->operand;
                int scope_type = ip->operand2;
                if (idx < vm_global_count) {
                    if (vm_global_types[idx] != 0) {
                        int target_type = vm_global_types[idx];
                        if (target_type == TOK_STRING && val.type == VAL_LIST) {
                            if (!try_convert_value(&val, TOK_STRING)) {
                                error(current_eval_line, "No se pudo convertir lista a string en asignación a global '%s'",
                                      vm_global_names[idx] ? vm_global_names[idx] : "?");
                            }
                        } else {
                            val = vm_convert_value(val, target_type);
                        }
                    }
                    vm_globals[idx] = copy_value_secure(val);
                    const char *gname = vm_global_entries[idx].name;
                    if (gname) {
                        Scope *target_scope = (scope_type == GLOBAL_SUPER) ? super_global_scope : global_scope;
                        VarEntry *e = scope_find(target_scope, gname);
                        if (e) {
                            scope_assign(target_scope, gname, copy_value_secure(val), 0);
                        } else {
                            int vtype = valtype_to_tokentype(val.type);
                            scope_define(target_scope, gname, vtype, copy_value_secure(val));
                        }
                    }
                } else {
                    error(current_eval_line, "Global inválido");
                }
                ip++;
                DISPATCH();
            }

            OP_ADD: {
                Value b = pop(), a = pop();
                if (a.type == VAL_STRING || b.type == VAL_STRING) {
                    char buf[128];
                    const char *sa = (a.type == VAL_STRING) ? a.data.sval : (snprintf(buf, sizeof(buf), "%d", a.data.ival), buf);
                    const char *sb = (b.type == VAL_STRING) ? b.data.sval : (snprintf(buf, sizeof(buf), "%d", b.data.ival), buf);
                    char *cat = malloc(strlen(sa) + strlen(sb) + 1);
                    strcpy(cat, sa); strcat(cat, sb);
                    Value res = val_string(cat);
                    free(cat);
                    push(res);
                } else if (a.type == VAL_INT && b.type == VAL_INT) {
                    push(val_int(a.data.ival + b.data.ival));
                } else {
                    double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                    double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                    push(val_float(av + bv));
                }
                ip++;
                DISPATCH();
            }
            OP_SUB: {
                Value b = pop(), a = pop();
                if (a.type == VAL_INT && b.type == VAL_INT) push(val_int(a.data.ival - b.data.ival));
                else push(val_float((a.type==VAL_INT?a.data.ival:a.data.fval) - (b.type==VAL_INT?b.data.ival:b.data.fval)));
                ip++;
                DISPATCH();
            }
            OP_MUL: {
                Value b = pop(), a = pop();
                if (a.type == VAL_INT && b.type == VAL_INT) push(val_int(a.data.ival * b.data.ival));
                else push(val_float((a.type==VAL_INT?a.data.ival:a.data.fval) * (b.type==VAL_INT?b.data.ival:b.data.fval)));
                ip++;
                DISPATCH();
            }
            OP_DIV: {
                Value b = pop(), a = pop();
                double av = (a.type==VAL_INT) ? a.data.ival : a.data.fval;
                double bv = (b.type==VAL_INT) ? b.data.ival : b.data.fval;
                if (bv == 0) error(current_eval_line, "División por cero");
                push(val_float(av / bv));
                ip++;
                DISPATCH();
            }
            OP_MOD: {
                Value b = pop(), a = pop();
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.data.ival == 0) error(current_eval_line, "Módulo por cero");
                    push(val_int(a.data.ival % b.data.ival));
                } else error(current_eval_line, "Módulo sólo para enteros");
                ip++;
                DISPATCH();
            }
            OP_NEG: {
                Value v = pop();
                if (v.type == VAL_INT) push(val_int(-v.data.ival));
                else if (v.type == VAL_FLOAT) push(val_float(-v.data.fval));
                else error(current_eval_line, "Negación no aplicable");
                ip++;
                DISPATCH();
            }

            OP_EQ: {
                Value b = pop(), a = pop();
                int eq = 0;
                if (a.type == b.type) {
                    switch (a.type) {
                        case VAL_INT: eq = (a.data.ival == b.data.ival); break;
                        case VAL_FLOAT: eq = (a.data.fval == b.data.fval); break;
                        case VAL_BOOL: eq = (a.data.bval == b.data.bval); break;
                        case VAL_STRING: eq = (strcmp(a.data.sval, b.data.sval) == 0); break;
                        default: eq = 0;
                    }
                }
                push(val_bool(eq));
                ip++;
                DISPATCH();
            }
            OP_NEQ: {
                Value b = pop(), a = pop();
                int neq = 1;
                if (a.type == b.type) {
                    switch (a.type) {
                        case VAL_INT: neq = (a.data.ival != b.data.ival); break;
                        case VAL_FLOAT: neq = (a.data.fval != b.data.fval); break;
                        case VAL_BOOL: neq = (a.data.bval != b.data.bval); break;
                        case VAL_STRING: neq = (strcmp(a.data.sval, b.data.sval) != 0); break;
                        default: neq = 1;
                    }
                }
                push(val_bool(neq));
                ip++;
                DISPATCH();
            }
            OP_LT: {
                Value b = pop(), a = pop();
                double av = (a.type==VAL_INT) ? a.data.ival : a.data.fval;
                double bv = (b.type==VAL_INT) ? b.data.ival : b.data.fval;
                push(val_bool(av < bv));
                ip++;
                DISPATCH();
            }
            OP_GT: {
                Value b = pop(), a = pop();
                double av = (a.type==VAL_INT) ? a.data.ival : a.data.fval;
                double bv = (b.type==VAL_INT) ? b.data.ival : b.data.fval;
                push(val_bool(av > bv));
                ip++;
                DISPATCH();
            }
            OP_LE: {
                Value b = pop(), a = pop();
                double av = (a.type==VAL_INT) ? a.data.ival : a.data.fval;
                double bv = (b.type==VAL_INT) ? b.data.ival : b.data.fval;
                push(val_bool(av <= bv));
                ip++;
                DISPATCH();
            }
            OP_GE: {
                Value b = pop(), a = pop();
                double av = (a.type==VAL_INT) ? a.data.ival : a.data.fval;
                double bv = (b.type==VAL_INT) ? b.data.ival : b.data.fval;
                push(val_bool(av >= bv));
                ip++;
                DISPATCH();
            }

            OP_AND: {
                Value b = pop(), a = pop();
                int truthy_a = (a.type == VAL_BOOL && a.data.bval) || (a.type == VAL_INT && a.data.ival != 0) || (a.type == VAL_FLOAT && a.data.fval != 0.0) || (a.type == VAL_STRING && a.data.sval[0]);
                int truthy_b = (b.type == VAL_BOOL && b.data.bval) || (b.type == VAL_INT && b.data.ival != 0) || (b.type == VAL_FLOAT && b.data.fval != 0.0) || (b.type == VAL_STRING && b.data.sval[0]);
                push(val_bool(truthy_a && truthy_b));
                ip++;
                DISPATCH();
            }
            OP_OR: {
                Value b = pop(), a = pop();
                int truthy_a = (a.type == VAL_BOOL && a.data.bval) || (a.type == VAL_INT && a.data.ival != 0) || (a.type == VAL_FLOAT && a.data.fval != 0.0) || (a.type == VAL_STRING && a.data.sval[0]);
                int truthy_b = (b.type == VAL_BOOL && b.data.bval) || (b.type == VAL_INT && b.data.ival != 0) || (b.type == VAL_FLOAT && b.data.fval != 0.0) || (b.type == VAL_STRING && b.data.sval[0]);
                push(val_bool(truthy_a || truthy_b));
                ip++;
                DISPATCH();
            }
            OP_NOT: {
                Value v = pop();
                int truthy = (v.type == VAL_BOOL && v.data.bval) || (v.type == VAL_INT && v.data.ival != 0) || (v.type == VAL_FLOAT && v.data.fval != 0.0) || (v.type == VAL_STRING && v.data.sval[0]);
                push(val_bool(!truthy));
                ip++;
                DISPATCH();
            }

            OP_CALL_BUILTIN: {
                int builtin_idx = ip->operand;
                int arg_count = ip->operand2;
                if (!call_builtin(builtin_idx, arg_count)) error(current_eval_line, "Error en builtin");
                ip++;
                DISPATCH();
            }

            OP_CALL_USER: {
                int func_index = ip->operand;
                int arg_count = ip->operand2;
                (void)arg_count;
                Chunk *func_code = vm_get_user_function(func_index);
                if (!func_code) error(current_eval_line, "Función de usuario no encontrada");
                Instruction *saved_ip = ip;
                Value result = vm_run(func_code);
                push(result);
                ip = saved_ip + 1;
                DISPATCH();
            }

            OP_RETURN: {
                Value ret = peek(0);
                return ret;
            }

            OP_JUMP_IF_FALSE: {
                Value v = pop();
                int truthy = (v.type == VAL_BOOL && v.data.bval) || (v.type == VAL_INT && v.data.ival != 0) || (v.type == VAL_FLOAT && v.data.fval != 0.0) || (v.type == VAL_STRING && v.data.sval[0]);
                if (!truthy) ip += ip->operand;
                else ip++;
                DISPATCH();
            }
            OP_JUMP:
            ip += ip->operand;
            DISPATCH();

            OP_DUP:  push(peek(0)); ip++; DISPATCH();
            OP_POP:  pop(); ip++; DISPATCH();

            OP_NEW_LIST:   push(val_list_empty()); ip++; DISPATCH();
            OP_LIST_APPEND: {
                Value item = pop();
                Value *list = sp - 1;
                val_list_append(list, item);
                ip++;
                DISPATCH();
            }

            OP_NEW_MAP:   push(val_map_empty()); ip++; DISPATCH();
            OP_MAP_SET: {
                Value val = pop();
                Value key = pop();
                Value map = peek(0);
                if (map.type != VAL_MAP) {
                    error(current_eval_line, "Se esperaba un mapa", map.type);
                }
                if (key.type != VAL_STRING) {
                    error(current_eval_line, "La clave de un mapa debe ser string");
                }
                val_map_set(&map, key.data.sval, val);
                ip++;
                DISPATCH();
            }

            OP_INDEX: {
                Value idx = pop();
                Value base = pop();
                if (base.type == VAL_LIST) {
                    if (idx.type != VAL_INT) error(current_eval_line, "Índice de lista debe ser entero");
                    int i = idx.data.ival;
                    if (i < 1 || i > base.data.list.count) error(current_eval_line, "Índice fuera de rango");
                    push(base.data.list.items[i-1]);
                } else if (base.type == VAL_STRING) {
                    if (idx.type != VAL_INT) error(current_eval_line, "Índice de string debe ser entero");
                    int i = idx.data.ival;
                    size_t len = strlen(base.data.sval);
                    if (i < 1 || (size_t)i > len) error(current_eval_line, "Índice de string fuera de rango");
                    char c[2] = {base.data.sval[i-1], 0};
                    push(val_string(c));
                } else if (base.type == VAL_MAP) {
                    if (idx.type != VAL_STRING) error(current_eval_line, "La clave de un mapa debe ser string");
                    Value result = val_map_get(base, idx.data.sval);
                    push(result);
                } else
                    error(current_eval_line, "Indexación no soportada");
                ip++;
                DISPATCH();
            }
            OP_INDEX_ASSIGN:
            error(current_eval_line, "Asignación con índice no implementada en VM (usar OP_INTERPRET_NODE)");
            ip++;
            DISPATCH();

            OP_EMBEDDED_CMD: {
                Value cmd_val = chunk->constants[ip->operand];
                const char *cmd = cmd_val.data.sval;
                char *expanded = expand_command_vm(chunk, locals, cmd);
                int ret = execute_embedded(expanded);
                if (ret == -1) error(current_eval_line, "Comando embebido falló");
                free(expanded);
                ip++;
                DISPATCH();
            }
            OP_SHELL_CMD: {
                Value cmd_val = chunk->constants[ip->operand];
                const char *cmd = cmd_val.data.sval;
                char *expanded = expand_command_vm(chunk, locals, cmd);
                int ret = run_shell_command(expanded);
                if (ret != 0) error(current_eval_line, "Comando shell falló (código %d)", ret);
                free(expanded);
                ip++;
                DISPATCH();
            }
            OP_FLAGS:
            error(current_eval_line, "flags no soportados en VM");
            ip++;
            DISPATCH();

            OP_CMD_ASSIGN: {
                Value cmd_val = chunk->constants[ip->operand];
                const char *cmd = cmd_val.data.sval;
                char *expanded = expand_command_vm(chunk, locals, cmd);
                FILE *fp = NULL;
                char *temp_path = NULL;

                size_t cmd_len = strlen(expanded);
                if (cmd_len >= 2 && expanded[0] == '!' && expanded[cmd_len-1] == '!') {
                    char *trimmed = strdup(expanded + 1);
                    trimmed[strlen(trimmed)-1] = '\0';
                    fp = popen_embedded_with_path(trimmed, "r", &temp_path);
                    free(trimmed);
                } else {
                    fp = popen(expanded, "r");
                }
                free(expanded);

                if (!fp) error(current_eval_line, "Error al ejecutar comando: %s", cmd);
                char buf[4096];
                char *out = strdup("");
                while (fgets(buf, sizeof(buf), fp)) {
                    out = realloc(out, strlen(out) + strlen(buf) + 1);
                    strcat(out, buf);
                }
                int status = pclose(fp);
                if (status != 0) error(current_eval_line, "Comando falló: %s", cmd);

                if (temp_path) {
                    unlink(temp_path);
                    free(temp_path);
                }

                size_t len = strlen(out);
                if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';

                int slot = ip->operand2;
                int expected_type = chunk->local_types[slot];
                Value final_val;

                if (expected_type == TOK_LIST) {
                    Value list = val_list_empty();
                    char *dup = strdup(out);
                    char *saveptr;
                    char *line = strtok_r(dup, "\n", &saveptr);
                    while (line) {
                        val_list_append(&list, val_string(line));
                        line = strtok_r(NULL, "\n", &saveptr);
                    }
                    free(dup);
                    free(out);
                    final_val = list;
                } else {
                    final_val = val_string(out);
                    free(out);
                }

                locals[slot] = final_val;

                if (slot < chunk->local_count && chunk->local_names[slot]) {
                    const char *name = chunk->local_names[slot];
                    VarEntry *e = scope_find(current_scope, name);
                    if (e) {
                        scope_assign(current_scope, name, final_val, 0);
                    } else {
                        int vtype = valtype_to_tokentype(final_val.type);
                        scope_define(current_scope, name, vtype, final_val);
                    }
                }

                ip++;
                DISPATCH();
            }

            OP_INTERPRET_NODE: {
                ASTNode *node = (ASTNode*)chunk->constants[ip->operand].data.ptr;
                Scope *temp_scope = scope_new(global_scope, NULL);
                for (int i = 0; i < chunk->local_count; i++) {
                    if (chunk->local_names[i]) {
                        scope_define(temp_scope, chunk->local_names[i],
                                     valtype_to_tokentype(locals[i].type), locals[i]);
                    }
                }
                Scope *old_scope = current_scope;
                current_scope = temp_scope;
                if (ip->operand2 == 0) {
                    NodeList block = { &node, 1, 1 };
                    exec_block(&block);
                } else {
                    Value result = eval_expr(node);
                    push(result);
                }
                // Copiar de vuelta las variables del scope temporal a locales
                for (int i = 0; i < chunk->local_count; i++) {
                    VarEntry *var = scope_find(temp_scope, chunk->local_names[i]);
                    if (var) locals[i] = var->value;
                }

                // Sincronizar variables globales con vm_globals y global_scope
                for (VarEntry *var = temp_scope->vars; var; var = var->next) {
                    VarEntry *existing = scope_find(global_scope, var->name);
                    if (existing) {
                        scope_assign(global_scope, var->name, var->value, 0);
                    } else {
                        scope_define(global_scope, var->name, var->vtype, var->value);
                    }
                    int gidx = vm_find_global_index(var->name);
                    if (gidx >= 0) {
                        vm_globals[gidx] = copy_value_secure(var->value);
                    }
                }

                // Sincronizar global_scope completo con vm_globals
                for (VarEntry *var = global_scope->vars; var; var = var->next) {
                    int gidx = vm_find_global_index(var->name);
                    if (gidx < 0) {
                        gidx = vm_register_global(var->name, GLOBAL_SCRIPT, var->vtype);
                    }
                    if (gidx >= 0) {
                        vm_globals[gidx] = copy_value_secure(var->value);
                    }
                }

                // Sincronizar super_global_scope con vm_globals
                for (VarEntry *var = super_global_scope->vars; var; var = var->next) {
                    int gidx = vm_find_global_index(var->name);
                    if (gidx < 0) {
                        gidx = vm_register_global(var->name, GLOBAL_SUPER, var->vtype);
                    }
                    if (gidx >= 0) {
                        vm_globals[gidx] = copy_value_secure(var->value);
                    }
                }

                current_scope = old_scope;
                scope_free(temp_scope);
                ip++;
                DISPATCH();
            }

            OP_LIST_INSERT: {
                Value idx = pop();
                Value elem = pop();
                Value list = pop();
                if (list.type != VAL_LIST) {
                    error(current_eval_line, "Se esperaba una lista para la operación de inserción");
                }
                int pos = (idx.type == VAL_INT) ? idx.data.ival : 1;
                if (pos < 1 || pos > list.data.list.count + 1) {
                    pos = list.data.list.count + 1;
                }
                Value new_list = val_list_copy(&list);
                val_list_append(&new_list, val_make_null());
                for (int i = new_list.data.list.count - 1; i > pos - 1; i--) {
                    new_list.data.list.items[i] = new_list.data.list.items[i - 1];
                }
                new_list.data.list.items[pos - 1] = elem;
                push(new_list);
                ip++;
                DISPATCH();
            }

            #ifdef USE_COMPUTED_GOTO
            #else
        }
    }
    #endif

    return val_make_null();
}
