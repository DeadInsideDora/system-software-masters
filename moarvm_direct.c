#include "moarvm_direct.h"
#include "moarvm_ops.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

typedef struct {
    uint16_t reg;
    MoarVMType type;
    const AST *type_info;
} Value;
typedef struct {
    const MoarVMProgramModel *model;
    const MoarVMFunctionModel *fn;
    MoarVMImage *image;
    uint16_t *variable_registers;
    uint16_t *register_types;
    size_t registers;
    uint32_t *frame_ids;
    uint32_t frame_start;
    uint32_t read_frame;
    char *error;
} Compiler;
static void fail(Compiler *c, const char *fmt, ...) {
    if (c->error)
        return;
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    c->error = strdup(message);
}
static uint16_t regtype(MoarVMType t) {
    return t == MV_INT ? 4 : t == MV_STRING ? 7 : t == MV_VOID ? 0 : 8;
}
static Value temp(Compiler *c, MoarVMType type) {
    Value v = {.type = type};
    if (c->registers >= UINT16_MAX) {
        fail(c, "too many registers in function");
        return v;
    }
    uint16_t *r = realloc(c->register_types, (c->registers + 1) * sizeof(*r));
    if (!r) {
        fail(c, "out of memory");
        return v;
    }
    c->register_types = r;
    v.reg = (uint16_t)c->registers;
    c->register_types[c->registers++] = regtype(type);
    return v;
}
static const MoarVMOp *op_info(uint16_t code) {
    for (size_t i = 0; i < sizeof(moarvm_ops) / sizeof(*moarvm_ops); i++)
        if (moarvm_ops[i].code == code)
            return &moarvm_ops[i];
    return NULL;
}
static void u16(Compiler *c, uint16_t n) {
    if (moarvm_image_append_bytecode_u16(c->image, n) == UINT32_MAX)
        fail(c, "bytecode allocation failed");
}
static void emit(Compiler *c, unsigned int code, ...) {
    if (c->error)
        return;
    const MoarVMOp *op = op_info(code);
    va_list ap;
    if (!op) {
        fail(c, "unknown compiler opcode %u", code);
        return;
    }
    u16(c, code);
    va_start(ap, code);
    for (const char *p = op->operands; *p; p++) {
        uint64_t arg = va_arg(ap, uint64_t);
        uint32_t result = 0;
        if (*p == 'r' || *p == 'h')
            result = moarvm_image_append_bytecode_u16(c->image, (uint16_t)arg);
        else if (*p == 'q')
            result = moarvm_image_append_bytecode_u64(c->image, arg);
        else
            result = moarvm_image_append_bytecode_u32(c->image, (uint32_t)arg);
        if (result == UINT32_MAX)
            fail(c, "bytecode allocation failed");
    }
    va_end(ap);
}
#define E0(c, op) emit(c, OP_##op)
#define E1(c, op, a) emit(c, OP_##op, (uint64_t)(a))
#define E2(c, op, a, b) emit(c, OP_##op, (uint64_t)(a), (uint64_t)(b))
#define E3(c, op, a, b, d) emit(c, OP_##op, (uint64_t)(a), (uint64_t)(b), (uint64_t)(d))
#define E4(c, op, a, b, d, e)                                                                      \
    emit(c, OP_##op, (uint64_t)(a), (uint64_t)(b), (uint64_t)(d), (uint64_t)(e))
static uint32_t string(Compiler *c, const char *text) {
    for (size_t i = 0; i < c->image->string_count; i++)
        if (!strcmp(c->image->strings[i], text))
            return (uint32_t)i;
    uint32_t n = moarvm_image_add_string(c->image, text);
    if (n == UINT32_MAX)
        fail(c, "string allocation failed");
    return n;
}
static Value integer(Compiler *c, int64_t n) {
    Value v = temp(c, MV_INT);
    E2(c, CONST_I64, v.reg, n);
    return v;
}
static Value textval(Compiler *c, const char *s) {
    Value v = temp(c, MV_STRING);
    E2(c, CONST_S, v.reg, string(c, s));
    return v;
}
static Value unary(Compiler *c, uint16_t op, MoarVMType type, Value a) {
    Value v = temp(c, type);
    emit(c, op, (uint64_t)v.reg, (uint64_t)a.reg);
    return v;
}
static Value binary(Compiler *c, uint16_t op, MoarVMType type, Value a, Value b) {
    Value v = temp(c, type);
    emit(c, op, (uint64_t)v.reg, (uint64_t)a.reg, (uint64_t)b.reg);
    return v;
}
static Value boot(Compiler *c, uint16_t op) {
    Value v = temp(c, MV_DYNAMIC);
    emit(c, op, (uint64_t)v.reg);
    return v;
}
static Value hash(Compiler *);
static void bind(Compiler *, Value, const char *, Value);
static Value fetch(Compiler *, Value, const char *);
static Value convert(Compiler *c, Value v, MoarVMType type) {
    if (v.type == type)
        return v;
    if (v.type == MV_VOID || type == MV_VOID) {
        fail(c, "void used as a value");
        return v;
    }
    if (type == MV_DYNAMIC) {
        if (v.type == MV_INT || v.type == MV_STRING) {
            Value t = boot(c, v.type == MV_INT ? OP_BOOTINT : OP_BOOTSTR), r = temp(c, MV_DYNAMIC);
            emit(c, v.type == MV_INT ? OP_BOX_I : OP_BOX_S, (uint64_t)r.reg, (uint64_t)v.reg,
                 (uint64_t)t.reg);
            return r;
        }
        v.type = MV_DYNAMIC;
        return v;
    }
    if (v.type == MV_DYNAMIC) {
        if (type == MV_INT || type == MV_STRING)
            return unary(c, type == MV_INT ? OP_UNBOX_I : OP_UNBOX_S, type, v);
        v.type = type;
        return v;
    }
    fail(c, "type mismatch: expected %s, got %s", moarvm_type_name(type), moarvm_type_name(v.type));
    return v;
}
static Value convert_type(Compiler *c, Value v, const AST *type_info) {
    return convert(c, v, moarvm_type_from_ast(type_info));
}
static uint32_t jump(Compiler *c, uint16_t op, Value condition) {
    if (op == OP_GOTO)
        emit(c, op, (uint64_t)0);
    else
        emit(c, op, (uint64_t)condition.reg, (uint64_t)0);
    return (uint32_t)c->image->bytecode_size - 4;
}
static void patch(Compiler *c, uint32_t at, uint32_t target) {
    moarvm_image_patch_u32(c->image, at, target - c->frame_start);
}
static void here(Compiler *c, uint32_t at) {
    patch(c, at, (uint32_t)c->image->bytecode_size);
}
static Value empty_array(Compiler *c, MoarVMType type) {
    Value t = boot(c, type == MV_STRING_ARRAY ? OP_BOOTSTRARRAY : OP_BOOTINTARRAY),
          v = temp(c, type);
    E2(c, CREATE, v.reg, t.reg);
    return v;
}
static Value default_value(Compiler *c, MoarVMType type) {
    if (type == MV_STRING)
        return textval(c, "");
    if (type == MV_INT_ARRAY || type == MV_STRING_ARRAY)
        return empty_array(c, type);
    return convert(c, integer(c, 0), type);
}
typedef struct {
    const MoarVMVariableModel *var;
    size_t slot;
} Binding;
static Binding binding(Compiler *c, const char *name) {
    Binding b = {0};
    b.var = moarvm_find_variable(c->fn, name);
    if (!b.var)
        fail(c, "unknown variable: %s", name);
    else
        b.slot = (size_t)(b.var - c->fn->vars);
    return b;
}
static Value load_binding(Compiler *c, Binding b) {
    if (!b.var)
        return (Value){0};
    Value v = temp(c, b.var->type);
    v.type_info = b.var->type_info;
    E2(c, SET, v.reg, c->variable_registers[b.slot]);
    return v;
}
static void store_binding(Compiler *c, Binding b, Value v) {
    if (b.var)
        E2(c, SET, c->variable_registers[b.slot], v.reg);
}
static Value expr(Compiler *, AST *);
static Value dispatch(Compiler *c, Value code, MoarVMType result, Value *args, size_t count) {
    Value v = {.type = MV_VOID};
    if (count >= UINT16_MAX) {
        fail(c, "too many arguments");
        return v;
    }
    uint8_t *flags = malloc(count + 1);
    flags[0] = 1;
    for (size_t i = 0; i < count; i++)
        flags[i + 1] = args[i].type == MV_INT ? 2 : args[i].type == MV_STRING ? 8 : 1;
    uint32_t cs = moarvm_image_add_callsite(c->image, flags, (uint16_t)(count + 1));
    free(flags);
    if (cs >= UINT16_MAX) {
        fail(c, "too many callsites");
        return v;
    }
    uint32_t name = string(c, "boot-code");
    if (result == MV_VOID)
        E2(c, DISPATCH_V, name, cs);
    else {
        v = temp(c, result);
        uint16_t op = result == MV_INT      ? OP_DISPATCH_I
                      : result == MV_STRING ? OP_DISPATCH_S
                                            : OP_DISPATCH_O;
        emit(c, op, (uint64_t)v.reg, (uint64_t)name, (uint64_t)cs);
    }
    u16(c, code.reg);
    for (size_t i = 0; i < count; i++)
        u16(c, args[i].reg);
    return v;
}
static Value frame_code(Compiler *c, uint32_t frame) {
    Value v = temp(c, MV_DYNAMIC);
    if (frame >= UINT16_MAX)
        fail(c, "too many frames");
    E2(c, GETCODE, v.reg, frame);
    return v;
}
static Value call_frame(Compiler *c, uint32_t frame, MoarVMType result, Value *args, size_t count) {
    return dispatch(c, frame_code(c, frame), result, args, count);
}
static Value user_call(Compiler *c, const MoarVMFunctionModel *f, AST *args) {
    size_t count = args ? (size_t)args->nchildren : 0;
    if (count != f->param_count) {
        fail(c, "%s expects %zu arguments, got %zu", f->source_name, f->param_count, count);
        return (Value){0};
    }
    Value *values = calloc(count ? count : 1, sizeof(*values));
    for (size_t i = 0; i < count; i++)
        values[i] = convert(c, expr(c, args->children[i]), f->vars[i].type);
    Value result = call_frame(c, c->frame_ids[f - c->model->functions], f->return_type, values, count);
    free(values);
    return result;
}
static char *unquote(Compiler *c, const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1), *q = out;
    for (size_t i = 1; i + 1 < len; i++) {
        char ch = s[i];
        if (ch == '\\') {
            ch = s[++i];
            switch (ch) {
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case '\\':
            case '\"':
            case '\'':
                break;
            case '0':
                fail(c, "embedded NUL in a string is not supported");
                break;
            default:
                fail(c, "unsupported escape sequence: \\%c", ch);
                break;
            }
        }
        *q++ = ch;
    }
    *q = 0;
    return out;
}
static Value literal(Compiler *c, const char *s) {
    if (s[0] == '"') {
        char *decoded = unquote(c, s);
        Value v = textval(c, decoded);
        free(decoded);
        return v;
    }
    if (s[0] == '\'') {
        if (!strcmp(s, "'\\0'"))
            return integer(c, 0);
        char *decoded = unquote(c, s);
        Value text = textval(c, decoded);
        free(decoded);
        return binary(c, OP_ORDAT, MV_INT, text, integer(c, 0));
    }
    if (!strcmp(s, "true"))
        return integer(c, 1);
    if (!strcmp(s, "false"))
        return integer(c, 0);
    int base = 10;
    const char *start = s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = s + 2;
    }
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
        start = s + 2;
    }
    char *end;
    errno = 0;
    long long n = strtoll(start, &end, base);
    if (errno == ERANGE || *end)
        fail(c, "integer literal out of range: %s", s);
    return integer(c, n);
}
static Value scalar_binary(Compiler *c, const char *op, Value a, Value b) {
    struct {
        const char *name;
        uint16_t integer_op, string_op;
    } operations[] = {{"+", OP_ADD_I, OP_CONCAT_S},
                      {"-", OP_SUB_I, 0},
                      {"*", OP_MUL_I, 0},
                      {"/", OP_DIV_I, 0},
                      {"%", OP_MOD_I, 0},
                      {"==", OP_EQ_I, OP_EQ_S},
                      {"!=", OP_NE_I, OP_NE_S},
                      {"<", OP_LT_I, OP_LT_S},
                      {"<=", OP_LE_I, OP_LE_S},
                      {">", OP_GT_I, OP_GT_S},
                      {">=", OP_GE_I, OP_GE_S},
                      {"&", OP_BAND_I, 0},
                      {"|", OP_BOR_I, 0}};
    uint16_t iop = 0, sop = 0;
    for (size_t i = 0; i < sizeof(operations) / sizeof(*operations); i++)
        if (!strcmp(op, operations[i].name)) {
            iop = operations[i].integer_op;
            sop = operations[i].string_op;
        }
    if (!iop) {
        fail(c, "unsupported operator %s", op);
        return a;
    }
    if (a.type == MV_DYNAMIC || b.type == MV_DYNAMIC) {
        if (!sop)
            return binary(c, iop, MV_INT, convert(c, a, MV_INT), convert(c, b, MV_INT));
        Value oa = convert(c, a, MV_DYNAMIC), ob = convert(c, b, MV_DYNAMIC);
        Value sa = unary(c, OP_ISSTR, MV_INT, oa), sb = unary(c, OP_ISSTR, MV_INT, ob);
        Value strings = binary(c, OP_BOR_I, MV_INT, sa, sb);
        Value result = temp(c, !strcmp(op, "+") ? MV_DYNAMIC : MV_INT);
        uint32_t numeric = jump(c, OP_UNLESS_I, strings);
        Value sv = binary(c, sop, !strcmp(op, "+") ? MV_STRING : MV_INT, convert(c, oa, MV_STRING),
                          convert(c, ob, MV_STRING));
        sv = convert(c, sv, result.type);
        E2(c, SET, result.reg, sv.reg);
        uint32_t done = jump(c, OP_GOTO, (Value){0});
        here(c, numeric);
        Value iv = binary(c, iop, MV_INT, convert(c, oa, MV_INT), convert(c, ob, MV_INT));
        iv = convert(c, iv, result.type);
        E2(c, SET, result.reg, iv.reg);
        here(c, done);
        return result;
    }
    if (a.type == MV_STRING && b.type == MV_STRING && sop)
        return binary(c, sop, !strcmp(op, "+") ? MV_STRING : MV_INT, a, b);
    return binary(c, iop, MV_INT, convert(c, a, MV_INT), convert(c, b, MV_INT));
}
static Value index_read(Compiler *c, Value base, Value index) {
    index = convert(c, index, MV_INT);
    if (base.type == MV_STRING)
        return binary(c, OP_ORDAT, MV_INT, base, index);
    if (base.type == MV_INT_ARRAY)
        return binary(c, OP_ATPOS_I, MV_INT, base, index);
    if (base.type == MV_STRING_ARRAY)
        return binary(c, OP_ATPOS_S, MV_STRING, base, index);
    if (base.type == MV_DYNAMIC) {
        Value out = temp(c, MV_INT), isstr = unary(c, OP_ISSTR, MV_INT, base);
        uint32_t array = jump(c, OP_UNLESS_I, isstr);
        Value v = binary(c, OP_ORDAT, MV_INT, convert(c, base, MV_STRING), index);
        E2(c, SET, out.reg, v.reg);
        uint32_t end = jump(c, OP_GOTO, (Value){0});
        here(c, array);
        v = binary(c, OP_ATPOS_I, MV_INT, base, index);
        E2(c, SET, out.reg, v.reg);
        here(c, end);
        return out;
    }
    fail(c, "indexing requires a string or array");
    return base;
}
static Value assignment(Compiler *c, AST *node) {
    AST *lhs = node->children[0];
    Value dst = {0}, base = {0}, index = {0}, previous = {0};
    Binding target = {0};
    const AST *target_type = NULL;
    if (lhs->type == NODE_IDENTIFIER) {
        target = binding(c, lhs->label);
        if (!target.var)
            return dst;
        dst.type = target.var->type;
        dst.type_info = target.var->type_info;
        target_type = target.var->type_info;
    } else if (lhs->type == NODE_INDEX) {
        base = expr(c, lhs->children[0]);
        index = convert(c, expr(c, lhs->children[1]->children[0]), MV_INT);
        if (base.type != MV_INT_ARRAY && base.type != MV_STRING_ARRAY &&
            base.type != MV_DYNAMIC)
            fail(c, "array assignment requires a mutable array");
        dst.type = base.type == MV_STRING_ARRAY ? MV_STRING : MV_INT;
    } else {
        fail(c, "assignment target must be a variable or array element");
        return dst;
    }
    if (strcmp(node->label, "=")) {
        if (lhs->type == NODE_INDEX)
            previous = index_read(c, base, index);
        else {
            previous = load_binding(c, target);
        }
    }
    Value rhs = expr(c, node->children[1]);
    if (strcmp(node->label, "=")) {
        char op[2] = {node->label[0], 0};
        rhs = scalar_binary(c, op, previous, rhs);
    }
    rhs = target_type || lhs->type == NODE_IDENTIFIER ? convert_type(c, rhs, target_type)
                                                      : convert(c, rhs, dst.type);
    if (lhs->type == NODE_IDENTIFIER)
        store_binding(c, target, rhs);
    else
        emit(c,
             dst.type == MV_STRING ? OP_BINDPOS_S : OP_BINDPOS_I,
             (uint64_t)base.reg, (uint64_t)index.reg, (uint64_t)rhs.reg);
    return rhs;
}
static void print_value(Compiler *c, Value arg, int as_character) {
    if (arg.type == MV_DYNAMIC) {
        Value isstr = unary(c, OP_ISSTR, MV_INT, arg);
        uint32_t numeric = jump(c, OP_UNLESS_I, isstr);
        Value s = convert(c, arg, MV_STRING);
        E1(c, PRINT, s.reg);
        uint32_t done = jump(c, OP_GOTO, (Value){0});
        here(c, numeric);
        Value n = convert(c, arg, MV_INT);
        s = unary(c, as_character ? OP_CHR : OP_COERCE_IS, MV_STRING, n);
        E1(c, PRINT, s.reg);
        here(c, done);
    } else {
        Value s = arg.type == MV_STRING ? arg
                                        : unary(c, as_character ? OP_CHR : OP_COERCE_IS, MV_STRING,
                                                convert(c, arg, MV_INT));
        E1(c, PRINT, s.reg);
    }
}
static Value length(Compiler *c, Value a) {
    if (a.type == MV_STRING)
        return unary(c, OP_CHARS, MV_INT, a);
    if (a.type == MV_INT_ARRAY || a.type == MV_STRING_ARRAY)
        return unary(c, OP_ELEMS, MV_INT, a);
    if (a.type == MV_DYNAMIC) {
        Value out = temp(c, MV_INT), isstr = unary(c, OP_ISSTR, MV_INT, a);
        uint32_t array = jump(c, OP_UNLESS_I, isstr);
        Value n = unary(c, OP_CHARS, MV_INT, convert(c, a, MV_STRING));
        E2(c, SET, out.reg, n.reg);
        uint32_t end = jump(c, OP_GOTO, (Value){0});
        here(c, array);
        n = unary(c, OP_ELEMS, MV_INT, a);
        E2(c, SET, out.reg, n.reg);
        here(c, end);
        return out;
    }
    fail(c, "len expects string or array");
    return a;
}
static Value call(Compiler *c, AST *node) {
    AST *callee = node->children[0], *args = node->children[1];
    if (callee->type != NODE_IDENTIFIER) {
        fail(c, "call requires a named function");
        return (Value){0};
    }
    const char *name = callee->label;
    size_t n = (size_t)args->nchildren;
    const MoarVMFunctionModel *f = moarvm_find_function(c->model, name);
    if (f && f->has_body)
        return user_call(c, f, args);
    if (!strcmp(name, "read_int") || !strcmp(name, "read_str")) {
        f = moarvm_find_function(c->model,
                                 !strcmp(name, "read_int") ? "__spo_read_int" : "__spo_read_str");
        return user_call(c, f, args);
    }
    if (!strcmp(name, "read")) {
        if (n)
            fail(c, "read expects no arguments");
        return call_frame(c, c->read_frame, MV_INT, NULL, 0);
    }
    if (!strcmp(name, "write") || !strcmp(name, "print") || !strcmp(name, "print_int") ||
        !strcmp(name, "put_int")) {
        if (n != 1) {
            fail(c, "%s expects one argument", name);
            return (Value){.type = MV_INT};
        }
        Value v = expr(c, args->children[0]);
        if (!strcmp(name, "print_int") || !strcmp(name, "put_int"))
            v = convert(c, v, MV_INT);
        print_value(c, v, !strcmp(name, "write"));
        return integer(c, 0);
    }
    if (!strcmp(name, "put_str")) {
        if (n != 1 && n != 2) {
            fail(c, "put_str expects one or two arguments");
            return (Value){.type = MV_INT};
        }
        Value v = convert(c, expr(c, args->children[0]), MV_STRING);
        if (n == 2) {
            Value count = convert(c, expr(c, args->children[1]), MV_INT), zero = integer(c, 0),
                  part = temp(c, MV_STRING);
            E4(c, SUBSTR_S, part.reg, v.reg, zero.reg, count.reg);
            v = part;
        }
        E1(c, PRINT, v.reg);
        return integer(c, 0);
    }
    if (!strcmp(name, "len")) {
        if (n != 1) {
            fail(c, "len expects one argument");
            return (Value){.type = MV_INT};
        }
        return length(c, expr(c, args->children[0]));
    }
    if (!strcmp(name, "new_int_array") || !strcmp(name, "new_string_array")) {
        if (n != 1) {
            fail(c, "%s expects one argument", name);
            return (Value){.type = MV_INT};
        }
        Value size = convert(c, expr(c, args->children[0]), MV_INT);
        Value array =
            empty_array(c, !strcmp(name, "new_int_array") ? MV_INT_ARRAY : MV_STRING_ARRAY);
        E2(c, SETELEMSPOS, array.reg, size.reg);
        return array;
    }
    fail(c, "undefined function: %s", name);
    return (Value){.type = MV_INT};
}
static Value expr(Compiler *c, AST *n) {
    if (!n || c->error)
        return (Value){.type = MV_INT};
    switch (n->type) {
    case NODE_LITERAL:
        return literal(c, n->label);
    case NODE_IDENTIFIER: {
        if (moarvm_find_variable(c->fn, n->label))
            return load_binding(c, binding(c, n->label));
        fail(c, "unknown variable: %s", n->label);
        return (Value){0};
    }
    case NODE_CALL:
        return call(c, n);
    case NODE_INDEX: {
        Value base = expr(c, n->children[0]), index = expr(c, n->children[1]->children[0]);
        return index_read(c, base, index);
    }
    case NODE_UNARY: {
        Value a = convert(c, expr(c, n->children[0]), MV_INT);
        if (!strcmp(n->label, "+"))
            return a;
        return unary(c, !strcmp(n->label, "-") ? OP_NEG_I : OP_NOT_I, MV_INT, a);
    }
    case NODE_BINARY: {
        const char *op = n->label;
        if (!strcmp(op, "=") || !strcmp(op, "+=") || !strcmp(op, "-=") || !strcmp(op, "*=") ||
            !strcmp(op, "/=") || !strcmp(op, "%="))
            return assignment(c, n);
        Value a = expr(c, n->children[0]);
        if (!strcmp(op, "&&") || !strcmp(op, "||")) {
            a = convert(c, a, MV_INT);
            Value result = integer(c, !strcmp(op, "||"));
            uint32_t done = jump(c, !strcmp(op, "||") ? OP_IF_I : OP_UNLESS_I, a);
            Value b = convert(c, expr(c, n->children[1]), MV_INT), zero = integer(c, 0),
                  test = binary(c, OP_NE_I, MV_INT, b, zero);
            E2(c, SET, result.reg, test.reg);
            here(c, done);
            return result;
        }
        Value b = expr(c, n->children[1]);
        return scalar_binary(c, op, a, b);
    }
    default:
        fail(c, "unsupported expression");
        return (Value){.type = MV_INT};
    }
}
static void return_value(Compiler *c, MoarVMType type, Value v) {
    if (type == MV_VOID) {
        E0(c, RETURN);
        return;
    }
    v = convert(c, v, type);
    emit(c,
         type == MV_INT      ? OP_RETURN_I
         : type == MV_STRING ? OP_RETURN_S
                             : OP_RETURN_O,
         (uint64_t)v.reg);
}
static void begin_frame(Compiler *c) {
    free(c->register_types);
    c->register_types = NULL;
    c->registers = 0;
    c->frame_start = (uint32_t)c->image->bytecode_size;
}
static void end_frame(Compiler *c, const char *name) {
    MoarVMFrame f;
    moarvm_frame_init(&f);
    f.bytecode_offset = c->frame_start;
    f.bytecode_size = (uint32_t)c->image->bytecode_size - c->frame_start;
    f.name_string_index = string(c, name);
    f.cuuid_string_index = f.name_string_index;
    f.outer_index = (uint16_t)c->image->frame_count;
    if (moarvm_frame_set_local_types(&f, c->register_types, (uint32_t)c->registers) != 0 ||
        moarvm_image_add_frame(c->image, &f) == UINT32_MAX)
        fail(c, "frame allocation failed");
    moarvm_frame_clear(&f);
}
static void compile_function(Compiler *c, const MoarVMFunctionModel *f) {
    c->fn = f;
    begin_frame(c);
    if (f->var_count > UINT16_MAX) {
        fail(c, "too many variables");
        return;
    }
    if (f->param_count > INT16_MAX) {
        fail(c, "too many parameters");
        return;
    }
    free(c->variable_registers);
    c->variable_registers = calloc(f->var_count ? f->var_count : 1, sizeof(*c->variable_registers));
    for (size_t i = 0; i < f->var_count; i++)
        c->variable_registers[i] = temp(c, f->vars[i].type).reg;
    E2(c, CHECKARITY, f->param_count, f->param_count);
    for (size_t i = 0; i < f->param_count; i++) {
        uint16_t op = f->vars[i].type == MV_INT ? OP_PARAM_RP_I
                      : f->vars[i].type == MV_STRING ? OP_PARAM_RP_S : OP_PARAM_RP_O;
        emit(c, op, (uint64_t)c->variable_registers[i], (uint64_t)i);
    }
    E0(c, PARAMNAMESUSED);
    for (size_t i = f->param_count; i < f->var_count; i++) {
        Value initial = default_value(c, f->vars[i].type);
        E2(c, SET, c->variable_registers[i], initial.reg);
    }
    const FunctionCFG *cfg = f->cfg;
    uint32_t *offsets = calloc((size_t)cfg->nnodes, sizeof(*offsets));
    typedef struct {
        uint32_t at;
        int target;
    } Edge;
    Edge *edges = calloc((size_t)cfg->nnodes * 2 + 1, sizeof(*edges));
    size_t nedges = 0;
    edges[nedges++] = (Edge){jump(c, OP_GOTO, (Value){0}), cfg->entry->id};
    for (int i = 0; i < cfg->nnodes && !c->error; i++) {
        CFGNode *n = cfg->nodes[i];
        offsets[n->id] = (uint32_t)c->image->bytecode_size;
        if (n == cfg->exit) {
            Value value = f->return_type == MV_VOID ? (Value){.type = MV_VOID}
                          : default_value(c, f->return_type);
            return_value(c, f->return_type, value);
            continue;
        }
        if (n->kind == CFG_RETURN) {
            if (f->return_type == MV_VOID && n->operation) {
                fail(c, "%s: void function returns a value", f->source_name);
                break;
            }
            if (f->return_type != MV_VOID && !n->operation && f->return_type != MV_DYNAMIC) {
                fail(c, "%s: return requires a value", f->source_name);
                break;
            }
            Value v = n->operation                ? expr(c, n->operation)
                      : f->return_type == MV_VOID ? (Value){.type = MV_VOID}
                                                  : default_value(c, f->return_type);
            return_value(c, f->return_type, v);
            continue;
        }
        if (n->kind == CFG_CONDITION) {
            Value condition = convert(c, expr(c, n->operation), MV_INT);
            edges[nedges++] = (Edge){jump(c, OP_UNLESS_I, condition), n->nextConditional->id};
        } else if (n->kind == CFG_EXPR)
            expr(c, n->operation);
        else if (n->kind == CFG_DECL) {
            AST *decl = n->operation;
            Binding dst = binding(c, decl->label);
            Value value;
            AST *initializer = NULL;
            for (int j = 0; j < decl->nchildren; j++)
                if (decl->children[j]->type != NODE_TYPE)
                    initializer = decl->children[j];
            value = initializer ? convert_type(c, expr(c, initializer), dst.var->type_info)
                                : default_value(c, dst.var->type);
            store_binding(c, dst, value);
        }
        if (n->nextDefault)
            edges[nedges++] = (Edge){jump(c, OP_GOTO, (Value){0}), n->nextDefault->id};
    }
    for (size_t i = 0; i < nedges && !c->error; i++)
        patch(c, edges[i].at, offsets[edges[i].target]);
    free(edges);
    free(offsets);
    return_value(c, f->return_type,
                 f->return_type == MV_VOID ? (Value){.type = MV_VOID}
                                           : default_value(c, f->return_type));
    end_frame(c, f->source_name);
}
static Value hash(Compiler *c) {
    Value type = boot(c, OP_BOOTHASH);
    return unary(c, OP_CREATE, MV_DYNAMIC, type);
}
static void bind(Compiler *c, Value h, const char *key, Value v) {
    Value k = textval(c, key);
    v = convert(c, v, MV_DYNAMIC);
    E3(c, BINDKEY_O, h.reg, k.reg, v.reg);
}
static Value fetch(Compiler *c, Value h, const char *key) {
    Value k = textval(c, key);
    return binary(c, OP_ATKEY_O, MV_DYNAMIC, h, k);
}
static void compile_read(Compiler *c) {
    begin_frame(c);
    c->fn = NULL;
    E2(c, CHECKARITY, 0, 0);
    E0(c, PARAMNAMESUSED);
    Value symbol = textval(c, "__spo_io"), state = temp(c, MV_DYNAMIC);
    E2(c, GETCURHLLSYM, state.reg, symbol.reg);
    Value missing = unary(c, OP_ISNULL, MV_INT, state);
    uint32_t ready = jump(c, OP_UNLESS_I, missing);
    Value fresh = hash(c);
    E2(c, SET, state.reg, fresh.reg);
    Value how = boot(c, OP_KNOWHOW), repr = textval(c, "P6int"), byte_type = temp(c, MV_DYNAMIC);
    E3(c, NEWTYPE, byte_type.reg, how.reg, repr.reg);
    Value config = hash(c), details = hash(c);
    bind(c, details, "bits", integer(c, 8));
    bind(c, details, "unsigned", integer(c, 1));
    bind(c, config, "integer", details);
    E3(c, COMPOSETYPE, byte_type.reg, byte_type.reg, config.reg);
    repr = textval(c, "VMArray");
    Value array_type = temp(c, MV_DYNAMIC);
    E3(c, NEWTYPE, array_type.reg, how.reg, repr.reg);
    config = hash(c);
    details = hash(c);
    bind(c, details, "type", byte_type);
    bind(c, config, "array", details);
    E3(c, COMPOSETYPE, array_type.reg, array_type.reg, config.reg);
    Value buffer = unary(c, OP_CREATE, MV_DYNAMIC, array_type), handle = boot(c, OP_GETSTDIN);
    bind(c, state, "buffer", buffer);
    bind(c, state, "index", integer(c, 0));
    bind(c, state, "handle", handle);
    Value ignored = temp(c, MV_DYNAMIC);
    E3(c, BINDCURHLLSYM, ignored.reg, symbol.reg, state.reg);
    here(c, ready);
    buffer = fetch(c, state, "buffer");
    handle = fetch(c, state, "handle");
    Value index = convert(c, fetch(c, state, "index"), MV_INT),
          size = unary(c, OP_ELEMS, MV_INT, buffer);
    Value available = binary(c, OP_LT_I, MV_INT, index, size);
    uint32_t have_byte = jump(c, OP_IF_I, available);
    Value block_size = integer(c, 4096);
    E3(c, READ_FHB, handle.reg, buffer.reg, block_size.reg);
    E2(c, CONST_I64, index.reg, 0);
    size = unary(c, OP_ELEMS, MV_INT, buffer);
    uint32_t not_eof = jump(c, OP_IF_I, size);
    return_value(c, MV_INT, integer(c, 0));
    here(c, not_eof);
    here(c, have_byte);
    Value byte = binary(c, OP_ATPOS_I, MV_INT, buffer, index);
    Value next = binary(c, OP_ADD_I, MV_INT, index, integer(c, 1));
    bind(c, state, "index", next);
    return_value(c, MV_INT, byte);
    end_frame(c, "__spo_read");
}
int moarvm_generate_image(const MoarVMProgramModel *model, MoarVMImage *image, char **error) {

    Compiler c = {0};
    c.model = model;
    c.image = image;
    moarvm_image_init(image);
    if (error)
        *error = NULL;
    const MoarVMFunctionModel *main = moarvm_find_function(model, "main");
    if (!main || !main->has_body || main->param_count) {
        if (error)
            *error = strdup("main must be defined and take no parameters");
        return -1;
    }
    c.frame_ids = calloc(model->function_count ? model->function_count : 1, sizeof(*c.frame_ids));
    uint32_t count = 0;
    for (size_t i = 0; i < model->function_count; i++)
        c.frame_ids[i] = model->functions[i].has_body ? count++ : UINT32_MAX;
    c.read_frame = count;
    image->hll_name_string_index = string(&c, "spo");
    for (size_t i = 0; i < model->function_count && !c.error; i++)
        if (model->functions[i].has_body)
            compile_function(&c, &model->functions[i]);
    if (!c.error)
        compile_read(&c);
    if (!c.error) {
        begin_frame(&c);
        c.fn = NULL;
        call_frame(&c, c.frame_ids[main - model->functions], main->return_type, NULL, 0);
        E0(&c, RETURN);
        end_frame(&c, "__spo_start");
        image->main_frame_index = (uint32_t)image->frame_count;
    }
    free(c.frame_ids);
    free(c.variable_registers);
    free(c.register_types);
    if (c.error) {
        if (error)
            *error = c.error;
        else
            free(c.error);
        moarvm_image_free(image);
        return -1;
    }
    return 0;
}
const char *moarvm_runtime_source(void) {
    return "int __spo_read_int() {\n"
           " int c = read(); int sign = 1; int result = 0;\n"
           " while (c == 32 | c == 9 | c == 10 | c == 13) c = read();\n"
           " if (c == 45) { sign = -1; c = read(); } else if(c == 43) c = read();\n"
           " while (c >= 48 & c <= 57) { result = result * 10 + c - 48; c = read(); }\n"
           " return result * sign;\n"
           "}\n"
           "int __spo_read_str(int[] buf) {\n"
           " int c = read(); int n = 0;\n"
           " while (c == 10 | c == 13) c = read();\n"
           " while (c != 0 & c != 10 & c != 13) { buf[n] = c; n += 1; c = read(); }\n"
           " buf[n] = 0; return n;\n"
           "}\n";
}
