/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: vm/vm.c
*/

// #include "vm.h"
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
#include <math.h>
#include <stdint.h>

extern int current_eval_line;

#define STACK_MAX 4096
#define VM_MAX_COMMAND_OUTPUT (8u * 1024u * 1024u)
Value stack[STACK_MAX];
static Value *sp = stack;
static Value *active_locals = NULL;
static int active_local_count = 0;

static inline size_t vm_stack_depth(void) { return (size_t)(sp - stack); }

static void push(Value v) {
    if (sp >= stack + STACK_MAX)
        error(current_eval_line, "Desbordamiento de la pila de la VM");
    *sp++ = v;
}

static Value pop(void) {
    if (sp <= stack) {
        error(current_eval_line, "Underflow de la pila de la VM");
        return val_make_null();
    }
    return *--sp;
}

static Value peek(int dist) {
    if (dist < 0 || (size_t)dist >= vm_stack_depth()) {
        error(current_eval_line, "Acceso fuera de rango a la pila de la VM");
        return val_make_null();
    }
    return *(sp - 1 - dist);
}

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

/* --- Registro de globales --- */
int vm_register_global(const char *name, int scope_type, int vtype) {
    int existing = vm_find_global_index(name);
    if (existing >= 0) {
        if (vtype != 0 && vm_global_types[existing] == 0) {
            vm_global_types[existing] = vtype;
        }
        return existing;
    }

    if (vm_global_count >= MAX_GLOBALS) {
        error(0, "Demasiadas variables globales (máximo %d)", MAX_GLOBALS);
        return -1;
    }

    vm_global_entries[vm_global_count].name = strdup(name);
    vm_global_entries[vm_global_count].scope_type = scope_type;
    vm_globals[vm_global_count] = val_make_null();
    vm_global_types[vm_global_count] = vtype;
    vm_global_names[vm_global_count] = vm_global_entries[vm_global_count].name;

    Scope *target_scope = (scope_type == GLOBAL_SUPER) ? super_global_scope : global_scope;
    if (target_scope) {
        VarEntry *e = scope_find(target_scope, name);
        if (!e) {
            scope_define(target_scope, name, vtype, val_make_null());
        }
    }

    return vm_global_count++;
}

int vm_find_global_index(const char *name) {
    for (int i = 0; i < vm_global_count; i++) {
        if (vm_global_entries[i].name && strcmp(vm_global_entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* --- Registro de builtins --- */
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

/* --- Registro de funciones de usuario --- */
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
    if (index < 0 || index >= vm_builtin_count)
        error(current_eval_line, "Índice de builtin inválido");
    if (arg_count < 0 || (size_t)arg_count > vm_stack_depth())
        error(current_eval_line, "Argumentos insuficientes para builtin (se solicitaron %d, hay %zu)", arg_count, vm_stack_depth());
    Value *args = sp - arg_count;
    Value ret = vm_builtins[index](arg_count, args);
    for (int i = 0; i < arg_count; i++)
        value_free(&args[i]);
    sp -= arg_count;
    push(ret);
    return 1;
}

static char *expand_command_vm(Chunk *chunk, Value *locals, const char *cmd) {
    return expand_command_with_locals(cmd, chunk->local_names, locals, chunk->local_count);
}

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

extern Scope *global_scope;
extern Scope *super_global_scope;

Value vm_run(Chunk *chunk) {
    if (!chunk || chunk->code_count == 0) return val_make_null();

    Value *frame_sp = sp;
    Value *locals = NULL;
    if (chunk->local_count > 0) {
        locals = calloc((size_t)chunk->local_count, sizeof(*locals));
        if (!locals) error(current_eval_line, "No se pudo reservar memoria para variables locales");
    }
    active_locals = locals;
    active_local_count = chunk->local_count;
    Instruction *ip = chunk->code;

    for (;;) {
        current_eval_line = ip->line;
        switch (ip->op) {
            case OP_NOP:
                ip++;
                break;

            case OP_PUSH_INT:
                push(chunk->constants[ip->operand]);
                ip++;
                break;
            case OP_PUSH_FLOAT:
                push(chunk->constants[ip->operand]);
                ip++;
                break;
            case OP_PUSH_STRING:
                push(copy_value_secure(chunk->constants[ip->operand]));
                ip++;
                break;
            case OP_PUSH_BOOL:
                push(chunk->constants[ip->operand]);
                ip++;
                break;
            case OP_PUSH_NULL:
                push(val_make_null());
                ip++;
                break;

            case OP_LOAD_VAR: {
                int slot = ip->operand;
                if (slot < 0 || slot >= chunk->local_count)
                    error(current_eval_line, "Índice de local inválido: %d", slot);
                Value v = locals[slot];
                if (v.type == VAL_NULL) {
                    error(current_eval_line, "Variable local no definida");
                }
                if (v.type == VAL_STRING) {
                    v = val_string(v.data.sval);
                }
                push(v);
                ip++;
                break;
            }
            case OP_STORE_VAR: {
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
                if (slot < 0 || slot >= chunk->local_count)
                    error(current_eval_line, "Índice de local inválido: %d", slot);
                value_free(&locals[slot]);
                locals[slot] = val;
                if (slot < chunk->local_count && chunk->local_names[slot]) {
                    const char *name = chunk->local_names[slot];
                    VarEntry *e = scope_find(current_scope, name);
                    if (e) {
                        scope_assign(current_scope, name, val, 0);
                    } else {
                        int vtype = valtype_to_tokentype(val.type);
                        scope_define(current_scope, name, vtype, copy_value_secure(val));
                    }
                }
                ip++;
                break;
            }

            case OP_LOAD_GLOBAL: {
                int idx = ip->operand;
                if (idx < 0 || idx >= vm_global_count) {
                    error(current_eval_line, "Índice global inválido: %d (globales registradas: %d)", idx, vm_global_count);
                }
                Value v = vm_globals[idx];
                if (v.type == VAL_NULL) {
                    error(current_eval_line, "Variable global no definida: %s", vm_global_names[idx]);
                }
                push(copy_value_secure(v));
                ip++;
                break;
            }

            case OP_STORE_GLOBAL: {
                int idx = ip->operand;
                if (idx < 0 || idx >= vm_global_count) {
                    error(current_eval_line, "Índice global inválido: %d (globales registradas: %d)", idx, vm_global_count);
                }
                Value val = pop();
                int scope_type = ip->operand2;

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

                value_free(&vm_globals[idx]);
                vm_globals[idx] = copy_value_secure(val);

                const char *gname = vm_global_entries[idx].name;
                if (gname) {
                    Scope *target_scope = (scope_type == GLOBAL_SUPER) ? super_global_scope : global_scope;
                    VarEntry *e = scope_find(target_scope, gname);
                    if (e) {
                        scope_assign(target_scope, gname, val, current_eval_line);
                    } else {
                        int vtype = valtype_to_tokentype(val.type);
                        scope_define(target_scope, gname, vtype, copy_value_secure(val));
                    }
                }
                value_free(&val);

                ip++;
                break;
            }

            case OP_ADD: {
                Value b = pop(), a = pop();
                if (a.type == VAL_STRING || b.type == VAL_STRING) {
                    char abuf[128], bbuf[128];
                    const char *sa;
                    const char *sb;
                    if (a.type == VAL_STRING) {
                        sa = a.data.sval;
                    } else if (a.type == VAL_INT) {
                        snprintf(abuf, sizeof(abuf), "%d", a.data.ival);
                        sa = abuf;
                    } else if (a.type == VAL_FLOAT) {
                        snprintf(abuf, sizeof(abuf), "%.15g", a.data.fval);
                        sa = abuf;
                    } else {
                        snprintf(abuf, sizeof(abuf), "%s", a.data.bval ? "true" : "false");
                        sa = abuf;
                    }
                    if (b.type == VAL_STRING) {
                        sb = b.data.sval;
                    } else if (b.type == VAL_INT) {
                        snprintf(bbuf, sizeof(bbuf), "%d", b.data.ival);
                        sb = bbuf;
                    } else if (b.type == VAL_FLOAT) {
                        snprintf(bbuf, sizeof(bbuf), "%.15g", b.data.fval);
                        sb = bbuf;
                    } else {
                        snprintf(bbuf, sizeof(bbuf), "%s", b.data.bval ? "true" : "false");
                        sb = bbuf;
                    }
                    size_t sa_len = strlen(sa);
                    size_t sb_len = strlen(sb);
                    if (sa_len > SIZE_MAX - sb_len - 1)
                        error(current_eval_line, "Resultado de concatenación demasiado grande");
                    char *cat = malloc(sa_len + sb_len + 1);
                    if (!cat) error(current_eval_line, "Memoria insuficiente para concatenación");
                    memcpy(cat, sa, sa_len);
                    memcpy(cat + sa_len, sb, sb_len + 1);
                    Value res = val_string(cat);
                    free(cat);
                    value_free(&a);
                    value_free(&b);
                    push(res);
                } else if (a.type == VAL_INT && b.type == VAL_INT) {
                    Value res = val_int(a.data.ival + b.data.ival);
                    value_free(&a);
                    value_free(&b);
                    push(res);
                } else {
                    double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                    double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                    Value res = val_float(av + bv);
                    value_free(&a);
                    value_free(&b);
                    push(res);
                }
                ip++;
                break;
            }
            case OP_SUB: {
                Value b = pop(), a = pop();
                Value result;
                if (a.type == VAL_INT && b.type == VAL_INT) result = val_int(a.data.ival - b.data.ival);
                else result = val_float((a.type == VAL_INT ? a.data.ival : a.data.fval) - (b.type == VAL_INT ? b.data.ival : b.data.fval));
                value_free(&a);
                value_free(&b);
                push(result);
                ip++;
                break;
            }
            case OP_MUL: {
                Value b = pop(), a = pop();
                Value result;
                if (a.type == VAL_INT && b.type == VAL_INT) result = val_int(a.data.ival * b.data.ival);
                else result = val_float((a.type == VAL_INT ? a.data.ival : a.data.fval) * (b.type == VAL_INT ? b.data.ival : b.data.fval));
                value_free(&a);
                value_free(&b);
                push(result);
                ip++;
                break;
            }
            case OP_DIV: {
                Value b = pop(), a = pop();
                double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                if (bv == 0) {
                    value_free(&a);
                    value_free(&b);
                    error(current_eval_line, "División por cero");
                }
                Value result = val_float(av / bv);
                value_free(&a);
                value_free(&b);
                push(result);
                ip++;
                break;
            }
            case OP_MOD: {
                Value b = pop(), a = pop();
                if (a.type != VAL_INT || b.type != VAL_INT) {
                    value_free(&a);
                    value_free(&b);
                    error(current_eval_line, "Módulo sólo para enteros");
                }
                if (b.data.ival == 0) {
                    value_free(&a);
                    value_free(&b);
                    error(current_eval_line, "Módulo por cero");
                }
                Value result = val_int(a.data.ival % b.data.ival);
                value_free(&a);
                value_free(&b);
                push(result);
                ip++;
                break;
            }
            case OP_NEG: {
                Value v = pop();
                Value result;
                if (v.type == VAL_INT) result = val_int(-v.data.ival);
                else if (v.type == VAL_FLOAT) result = val_float(-v.data.fval);
                else {
                    value_free(&v);
                    error(current_eval_line, "Negación no aplicable");
                }
                value_free(&v);
                push(result);
                ip++;
                break;
            }

            case OP_EQ: {
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
                value_free(&a);
                value_free(&b);
                push(val_bool(eq));
                ip++;
                break;
            }
                        case OP_NEQ: {
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
                            value_free(&a);
                            value_free(&b);
                            push(val_bool(neq));
                            ip++;
                            break;
                        }
                                    case OP_LT: {
                                        Value b = pop(), a = pop();
                                        double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                                        double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                                        value_free(&a);
                                        value_free(&b);
                                        push(val_bool(av < bv));
                                        ip++;
                                        break;
                                    }
                                    case OP_GT: {
                                        Value b = pop(), a = pop();
                                        double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                                        double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                                        value_free(&a);
                                        value_free(&b);
                                        push(val_bool(av > bv));
                                        ip++;
                                        break;
                                    }
                                    case OP_LE: {
                                        Value b = pop(), a = pop();
                                        double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                                        double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                                        value_free(&a);
                                        value_free(&b);
                                        push(val_bool(av <= bv));
                                        ip++;
                                        break;
                                    }
                                    case OP_GE: {
                                        Value b = pop(), a = pop();
                                        double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                                        double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                                        value_free(&a);
                                        value_free(&b);
                                        push(val_bool(av >= bv));
                                        ip++;
                                        break;
                                    }

                                    case OP_AND: {
                                        Value b = pop(), a = pop();
                                        int truthy_a = (a.type == VAL_BOOL && a.data.bval) || (a.type == VAL_INT && a.data.ival != 0) || (a.type == VAL_FLOAT && a.data.fval != 0.0) || (a.type == VAL_STRING && a.data.sval[0]);
                                        int truthy_b = (b.type == VAL_BOOL && b.data.bval) || (b.type == VAL_INT && b.data.ival != 0) || (b.type == VAL_FLOAT && b.data.fval != 0.0) || (b.type == VAL_STRING && b.data.sval[0]);
                                        value_free(&a);
                                        value_free(&b);
                                        push(val_bool(truthy_a && truthy_b));
                                        ip++;
                                        break;
                                    }
                                    case OP_OR: {
                                        Value b = pop(), a = pop();
                                        int truthy_a = (a.type == VAL_BOOL && a.data.bval) || (a.type == VAL_INT && a.data.ival != 0) || (a.type == VAL_FLOAT && a.data.fval != 0.0) || (a.type == VAL_STRING && a.data.sval[0]);
                                        int truthy_b = (b.type == VAL_BOOL && b.data.bval) || (b.type == VAL_INT && b.data.ival != 0) || (b.type == VAL_FLOAT && b.data.fval != 0.0) || (b.type == VAL_STRING && b.data.sval[0]);
                                        value_free(&a);
                                        value_free(&b);
                                        push(val_bool(truthy_a || truthy_b));
                                        ip++;
                                        break;
                                    }
                                    case OP_NOT: {
                                        Value v = pop();
                                        int truthy = (v.type == VAL_BOOL && v.data.bval) || (v.type == VAL_INT && v.data.ival != 0) || (v.type == VAL_FLOAT && v.data.fval != 0.0) || (v.type == VAL_STRING && v.data.sval[0]);
                                        value_free(&v);
                                        push(val_bool(!truthy));
                                        ip++;
                                        break;
                                    }

                                    case OP_CALL_BUILTIN: {
                                        int builtin_idx = ip->operand;
                                        int arg_count = ip->operand2;
                                        if (!call_builtin(builtin_idx, arg_count)) error(current_eval_line, "Error en builtin");
                                        ip++;
                                        break;
                                    }

                                    case OP_CALL_USER: {
                                        int func_index = ip->operand;
                                        int arg_count = ip->operand2;
                                        (void)arg_count;
                                        Chunk *func_code = vm_get_user_function(func_index);
                                        if (!func_code) error(current_eval_line, "Función de usuario no encontrada");
                                        Instruction *saved_ip = ip;
                                        Value result = vm_run(func_code);
                                        push(result);
                                        ip = saved_ip + 1;
                                        break;
                                    }

                                    case OP_RETURN: {
                                        Value ret = vm_stack_depth() > (size_t)(frame_sp - stack)
                                        ? copy_value_secure(peek(0)) : val_make_null();
                                        while (sp > frame_sp) {
                                            --sp;
                                            value_free(sp);
                                        }
                                        for (int i = 0; i < chunk->local_count; i++)
                                            value_free(&locals[i]);
                                        free(locals);
                                        if (active_locals == locals) {
                                            active_locals = NULL;
                                            active_local_count = 0;
                                        }
                                        sp = frame_sp;
                                        return ret;
                                    }

                                    case OP_JUMP_IF_FALSE: {
                                        Value v = pop();
                                        int truthy = (v.type == VAL_BOOL && v.data.bval) || (v.type == VAL_INT && v.data.ival != 0) || (v.type == VAL_FLOAT && v.data.fval != 0.0) || (v.type == VAL_STRING && v.data.sval[0]);
                                        if (!truthy) ip += ip->operand;
                                        else ip++;
                                        break;
                                    }
                                    case OP_JUMP:
                                        ip += ip->operand;
                                        break;

                                    case OP_DUP:
                                        push(peek(0));
                                        ip++;
                                        break;
                                    case OP_POP: {
                                        Value discarded = pop();
                                        value_free(&discarded);
                                        ip++;
                                        break;
                                    }

                                    case OP_NEW_LIST:
                                        push(val_list_empty());
                                        ip++;
                                        break;
                                    case OP_LIST_APPEND: {
                                        Value item = pop();
                                        Value *list = sp - 1;
                                        val_list_append(list, item);
                                        ip++;
                                        break;
                                    }

                                    case OP_NEW_MAP:
                                        push(val_map_empty());
                                        ip++;
                                        break;
                                    case OP_MAP_SET: {
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
                                        value_free(&key);
                                        value_free(&val);
                                        ip++;
                                        break;
                                    }

                                    case OP_INDEX: {
                                        int name_idx = ip->operand2;
                                        const char *map_name = NULL;
                                        if (name_idx != 0) {
                                            Value name_val = chunk->constants[name_idx];
                                            if (name_val.type == VAL_STRING) {
                                                map_name = name_val.data.sval;
                                            }
                                        }

                                        Value idx = pop();
                                        Value base = pop();

                                        if (base.type == VAL_LIST) {
                                            if (idx.type != VAL_INT) {
                                                value_free(&idx);
                                                value_free(&base);
                                                error(current_eval_line, "Índice de lista debe ser entero");
                                            }
                                            int i = idx.data.ival;
                                            if (i < 1 || i > base.data.list.count) {
                                                value_free(&idx);
                                                value_free(&base);
                                                error(current_eval_line, "Índice fuera de rango");
                                            }
                                            push(copy_value_secure(base.data.list.items[i - 1]));
                                        }
                                        else if (base.type == VAL_STRING) {
                                            if (idx.type != VAL_INT) {
                                                value_free(&idx);
                                                value_free(&base);
                                                error(current_eval_line, "Índice de string debe ser entero");
                                            }
                                            int i = idx.data.ival;
                                            size_t len = strlen(base.data.sval);
                                            if (i < 1 || (size_t)i > len) {
                                                value_free(&idx);
                                                value_free(&base);
                                                error(current_eval_line, "Índice de string fuera de rango");
                                            }
                                            char c[2] = {base.data.sval[i - 1], 0};
                                            push(val_string(c));
                                        }
                                        else if (base.type == VAL_MAP) {
                                            if (idx.type != VAL_STRING) {
                                                value_free(&idx);
                                                value_free(&base);
                                                error(current_eval_line, "La clave de mapa debe ser string");
                                            }
                                            // Comprobar si la clave existe
                                            if (!val_map_has(base, idx.data.sval)) {
                                                if (map_name) {
                                                    error(current_eval_line,
                                                          "Clave '%s' no encontrada en el mapa '%s'",
                                                          idx.data.sval, map_name);
                                                } else {
                                                    error(current_eval_line,
                                                          "Clave '%s' no encontrada en el mapa",
                                                          idx.data.sval);
                                                }
                                            }
                                            Value result = val_map_get(base, idx.data.sval);
                                            push(result);
                                        }
                                        else {
                                            value_free(&idx);
                                            value_free(&base);
                                            error(current_eval_line, "Indexación no soportada");
                                        }

                                        value_free(&idx);
                                        value_free(&base);
                                        ip++;
                                        break;
                                    }
                                    case OP_INDEX_ASSIGN:
                                        error(current_eval_line, "Asignación con índice no implementada en VM (usar OP_INTERPRET_NODE)");
                                        ip++;
                                        break;

                                    case OP_EMBEDDED_CMD: {
                                        Value cmd_val = chunk->constants[ip->operand];
                                        const char *cmd = cmd_val.data.sval;
                                        char *expanded = expand_command_vm(chunk, locals, cmd);
                                        int ret = execute_embedded(expanded);
                                        if (ret == -1) error(current_eval_line, "Comando embebido falló");
                                        free(expanded);
                                        ip++;
                                        break;
                                    }
                                    case OP_SHELL_CMD: {
                                        Value cmd_val = chunk->constants[ip->operand];
                                        const char *cmd = cmd_val.data.sval;
                                        char *expanded = expand_command_vm(chunk, locals, cmd);
                                        int ret = run_shell_command(expanded);
                                        if (ret != 0) {
                                            int saved_ret = ret;
                                            free(expanded);
                                            error(current_eval_line, "Comando shell falló (código %d)", saved_ret);
                                        }
                                        free(expanded);
                                        ip++;
                                        break;
                                    }
                                    case OP_FLAGS:
                                        error(current_eval_line, "flags no soportados en VM");
                                        ip++;
                                        break;

                                    case OP_CMD_ASSIGN: {
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
                                            size_t old_len = strlen(out);
                                            size_t add_len = strlen(buf);
                                            if (old_len > VM_MAX_COMMAND_OUTPUT || add_len > VM_MAX_COMMAND_OUTPUT - old_len - 1) {
                                                free(out);
                                                pclose(fp);
                                                if (temp_path) { unlink(temp_path); free(temp_path); }
                                                error(current_eval_line, "Salida de comando demasiado grande");
                                            }
                                            char *tmp_out = realloc(out, old_len + add_len + 1);
                                            if (!tmp_out) {
                                                free(out);
                                                pclose(fp);
                                                if (temp_path) { unlink(temp_path); free(temp_path); }
                                                error(current_eval_line, "Memoria insuficiente para salida de comando");
                                            }
                                            out = tmp_out;
                                            memcpy(out + old_len, buf, add_len + 1);
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
                                        if (slot < 0 || slot >= chunk->local_count)
                                            error(current_eval_line, "Índice de local inválido: %d", slot);
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

                                        if (slot < 0 || slot >= chunk->local_count)
                                            error(current_eval_line, "Índice de local inválido: %d", slot);
                                        if (chunk->local_names[slot]) {
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
                                        break;
                                    }

                                    case OP_INTERPRET_NODE: {
                                        ASTNode *node = (ASTNode*)chunk->constants[ip->operand].data.ptr;
                                        Scope *temp_scope = scope_new(global_scope, NULL);
                                        for (int i = 0; i < chunk->local_count; i++) {
                                            if (chunk->local_names[i]) {
                                                scope_define(temp_scope, chunk->local_names[i],
                                                             valtype_to_tokentype(locals[i].type), copy_value_secure(locals[i]));
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
                                        for (int i = 0; i < chunk->local_count; i++) {
                                            VarEntry *var = scope_find(temp_scope, chunk->local_names[i]);
                                            if (var) {
                                                value_free(&locals[i]);
                                                locals[i] = copy_value_secure(var->value);
                                            }
                                        }

                                        for (VarEntry *var = temp_scope->vars; var; var = var->next) {
                                            VarEntry *existing = scope_find(global_scope, var->name);
                                            if (existing) {
                                                scope_assign(global_scope, var->name, var->value, 0);
                                            } else {
                                                scope_define(global_scope, var->name, var->vtype, copy_value_secure(var->value));
                                            }
                                            int gidx = vm_find_global_index(var->name);
                                            if (gidx >= 0) {
                                                value_free(&vm_globals[gidx]);
                                                vm_globals[gidx] = copy_value_secure(var->value);
                                            }
                                        }

                                        for (VarEntry *var = global_scope->vars; var; var = var->next) {
                                            int gidx = vm_find_global_index(var->name);
                                            if (gidx < 0) {
                                                gidx = vm_register_global(var->name, GLOBAL_SCRIPT, var->vtype);
                                            }
                                            if (gidx >= 0) {
                                                value_free(&vm_globals[gidx]);
                                                vm_globals[gidx] = copy_value_secure(var->value);
                                            }
                                        }

                                        for (VarEntry *var = super_global_scope->vars; var; var = var->next) {
                                            int gidx = vm_find_global_index(var->name);
                                            if (gidx < 0) {
                                                gidx = vm_register_global(var->name, GLOBAL_SUPER, var->vtype);
                                            }
                                            if (gidx >= 0) {
                                                value_free(&vm_globals[gidx]);
                                                vm_globals[gidx] = copy_value_secure(var->value);
                                            }
                                        }

                                        current_scope = old_scope;
                                        scope_free(temp_scope);
                                        ip++;
                                        break;
                                    }

                                    case OP_LIST_INSERT: {
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
                                        break;
                                    }

                                    case OP_POW: {
                                        Value b = pop(), a = pop();
                                        if (a.type == VAL_INT && b.type == VAL_INT && b.data.ival >= 0) {
                                            long result = 1;
                                            long base = a.data.ival;
                                            long exp = b.data.ival;
                                            for (long i = 0; i < exp; i++) result *= base;
                                            push(val_int((int)result));
                                        } else {
                                            double av = (a.type == VAL_INT) ? a.data.ival : a.data.fval;
                                            double bv = (b.type == VAL_INT) ? b.data.ival : b.data.fval;
                                            push(val_float(pow(av, bv)));
                                        }
                                        ip++;
                                        break;
                                    }

                                    case OP_REPEAT_LINE:
                                        error(current_eval_line, "OP_REPEAT_LINE no implementado en VM");
                                        ip++;
                                        break;

                                    default:
                                        error(current_eval_line, "Opcode desconocido: %d", ip->op);
                                        return val_make_null();
        }
    }
}


static void vm_chunk_free(Chunk *ch) {
    if (!ch) return;
    for (int i = 0; i < ch->const_count; i++) value_free(&ch->constants[i]);
    free(ch->constants);
    for (int i = 0; i < ch->local_count; i++) free(ch->local_names[i]);
    free(ch->local_names);
    free(ch->local_types);
    free(ch->code);
    free(ch);
}

void vm_cleanup_state(void) {
    if (active_locals) {
        for (int i = 0; i < active_local_count; i++)
            value_free(&active_locals[i]);
        free(active_locals);
        active_locals = NULL;
        active_local_count = 0;
    }
    while (sp > stack) {
        --sp;
        value_free(sp);
    }
    for (int i = 0; i < vm_global_count; i++) {
        value_free(&vm_globals[i]);
        free(vm_global_names[i]);
        vm_global_names[i] = NULL;
        vm_global_entries[i].name = NULL;
    }
    vm_global_count = 0;
    /* Ya no liberamos los Chunk aquí; se liberan en cleanup_runtime_state().
     *      Solo limpiamos los nombres para evitar uso posterior. */
    for (int i = 0; i < user_function_count; i++) {
        free(user_functions[i].name);
        user_functions[i].name = NULL;
        user_functions[i].code = NULL;   // no liberamos el Chunk
    }
    user_function_count = 0;
}
