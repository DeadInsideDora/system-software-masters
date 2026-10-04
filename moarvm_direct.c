#include "moarvm_direct.h"
#include "moarvm_ops.h"
#include "moarvm_ffi_abi.h"
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
static void require_object(Compiler *, Value, const char *);
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
    MoarVMType type = moarvm_type_from_ast(type_info);
    if (type == MV_FUNCTION) {
        if (v.type != MV_FUNCTION || (v.type_info && !moarvm_types_equal(v.type_info, type_info)))
            fail(c, "function type mismatch");
        v.type_info = type_info;
        return v;
    }
    if (type == MV_OBJECT) {
        if (v.type == MV_DYNAMIC)
            require_object(c, v, type_info->children[0]->label);
        else if (v.type != MV_OBJECT ||
                 (v.type_info &&
                  !objects_subtype(c->model->objects, v.type_info->children[0]->label,
                                   type_info->children[0]->label)))
            fail(c, "object type mismatch: expected %s", type_info->children[0]->label);
        v.type = type;
        v.type_info = type_info;
        return v;
    }
    if (type == MV_OBJECT_ARRAY) {
        if (v.type != type || (v.type_info && !moarvm_types_equal(v.type_info, type_info)))
            fail(c, "object array type mismatch");
        v.type_info = type_info;
        return v;
    }
    return convert(c, v, type);
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
    Value t = boot(c, type == MV_STRING_ARRAY   ? OP_BOOTSTRARRAY
                      : type == MV_OBJECT_ARRAY ? OP_BOOTARRAY
                                                : OP_BOOTINTARRAY),
          v = temp(c, type);
    E2(c, CREATE, v.reg, t.reg);
    return v;
}
static Value default_value(Compiler *c, MoarVMType type) {
    if (type == MV_FUNCTION || type == MV_OBJECT) {
        Value v = temp(c, type);
        E1(c, NULL, v.reg);
        return v;
    }
    if (type == MV_STRING)
        return textval(c, "");
    if (type == MV_INT_ARRAY || type == MV_STRING_ARRAY || type == MV_OBJECT_ARRAY)
        return empty_array(c, type);
    return convert(c, integer(c, 0), type);
}
typedef struct {
    const MoarVMVariableModel *var;
    unsigned depth;
    size_t slot;
} Binding;
static Binding binding(Compiler *c, const char *name) {
    Binding b = {0};
    b.var = moarvm_resolve_variable(c->model, c->fn, name, &b.depth, &b.slot);
    if (!b.var)
        fail(c, "unknown variable: %s", name);
    if (b.depth > UINT16_MAX || b.slot > UINT16_MAX)
        fail(c, "lexical address out of range");
    return b;
}
static Value load_binding(Compiler *c, Binding b) {
    if (!b.var)
        return (Value){0};
    Value v = temp(c, b.var->type);
    v.type_info = b.var->type_info;
    E3(c, GETLEX, v.reg, b.slot, b.depth);
    return v;
}
static void store_binding(Compiler *c, Binding b, Value v) {
    if (b.var)
        E3(c, BINDLEX, b.slot, b.depth, v.reg);
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
    Value v = temp(c, MV_FUNCTION);
    if (frame >= UINT16_MAX)
        fail(c, "too many frames");
    E2(c, GETCODE, v.reg, frame);
    return v;
}
static Value function_value(Compiler *c, const MoarVMFunctionModel *f) {
    if (!f->has_body)
        fail(c, "undefined function: %s", f->source_name);
    Value v = frame_code(c, c->frame_ids[f - c->model->functions]);
    v.type_info = f->value_type;
    return v;
}
static Value call_frame(Compiler *c, uint32_t frame, MoarVMType result, Value *args, size_t count) {
    return dispatch(c, frame_code(c, frame), result, args, count);
}
static Value invoke(Compiler *c, Value code, AST *args, const char *name) {
    if (code.type != MV_FUNCTION || !code.type_info) {
        fail(c, "call requires a function type: %s", name);
        return (Value){0};
    }
    const AST *signature = code.type_info;
    size_t count = args ? (size_t)args->nchildren : 0;
    size_t expected = (size_t)signature->nchildren - 1;
    if (count != expected) {
        fail(c, "%s expects %zu arguments, got %zu", name, expected, count);
        return (Value){0};
    }
    Value *values = calloc(count ? count : 1, sizeof(*values));
    for (size_t i = 0; i < count; i++)
        values[i] = convert_type(c, expr(c, args->children[i]), signature->children[i + 1]);
    const AST *result_type = signature->children[0];
    Value result = dispatch(c, code, moarvm_type_from_ast(result_type), values, count);
    result.type_info = result_type;
    free(values);
    return result;
}
static Value user_call(Compiler *c, const MoarVMFunctionModel *f, AST *args) {
    return invoke(c, function_value(c, f), args, f->source_name);
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
    if (!strcmp(s, "null"))
        return default_value(c, MV_OBJECT);
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
    if (a.type == MV_OBJECT || b.type == MV_OBJECT) {
        if (a.type != MV_OBJECT || b.type != MV_OBJECT || (strcmp(op, "==") && strcmp(op, "!="))) {
            fail(c, "objects support only reference equality with objects or null");
            return (Value){0};
        }
        Value equal = binary(c, OP_EQADDR, MV_INT, a, b);
        return !strcmp(op, "==") ? equal : unary(c, OP_NOT_I, MV_INT, equal);
    }
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
    if (base.type == MV_OBJECT_ARRAY) {
        Value v = binary(c, OP_ATPOS_O, MV_OBJECT, base, index);
        v.type_info = base.type_info ? base.type_info->children[0] : NULL;
        return v;
    }
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
static void runtime_error(Compiler *c, const char *message) {
    Value error = temp(c, MV_DYNAMIC), text = textval(c, message);
    E2(c, DIE, error.reg, text.reg);
}
static void nonnull(Compiler *c, Value object) {
    uint32_t ok = jump(c, OP_UNLESS_I, unary(c, OP_ISNULL, MV_INT, object));
    runtime_error(c, "member access on null object");
    here(c, ok);
}
static Value object_is(Compiler *c, Value value, const char *name) {
    if (value.type != MV_OBJECT && value.type != MV_DYNAMIC) {
        fail(c, "match requires an object");
        return integer(c, 0);
    }
    Value result = integer(c, 0);
    uint32_t null = jump(c, OP_IF_I, unary(c, OP_ISNULL, MV_INT, value));
    uint32_t other = jump(c, OP_UNLESS_I, unary(c, OP_ISHASH, MV_INT, value));
    Value tag = fetch(c, value, "@class");
    uint32_t untagged = jump(c, OP_IF_I, unary(c, OP_ISNULL, MV_INT, tag));
    tag = convert(c, tag, MV_INT);
    for (size_t i = 0; i < c->model->objects->count; i++)
        if (objects_subtype(c->model->objects, c->model->objects->classes[i].name, name)) {
            Value equal = binary(c, OP_EQ_I, MV_INT, tag, integer(c, (int64_t)i));
            Value both = binary(c, OP_BOR_I, MV_INT, result, equal);
            E2(c, SET, result.reg, both.reg);
        }
    here(c, untagged);
    here(c, other);
    here(c, null);
    return result;
}
static void require_object(Compiler *c, Value value, const char *name) {
    uint32_t null = jump(c, OP_IF_I, unary(c, OP_ISNULL, MV_INT, value));
    uint32_t valid = jump(c, OP_IF_I, object_is(c, value, name));
    runtime_error(c, "object type mismatch at runtime");
    here(c, valid);
    here(c, null);
}
static const ObjectClass *value_class(Compiler *c, Value base) {
    if (base.type != MV_OBJECT || !base.type_info) {
        fail(c, "member access requires a typed object");
        return NULL;
    }
    return objects_find(c->model->objects, base.type_info->children[0]->label);
}
static const AST *member_type(Compiler *c, Value base, const char *name, int writing) {
    const ObjectClass *class = value_class(c, base);
    const ObjectField *field = objects_field(class, name);
    if (field)
        return field->type;
    const ObjectMethod *method = objects_method(class, name);
    if (method && !writing)
        return method->type;
    fail(c, method ? "cannot assign to method: %s" : "unknown member: %s", name);
    return NULL;
}
static char *member_key(const char *name, int method) {
    size_t len = strlen(name) + 3;
    char *key = malloc(len);
    snprintf(key, len, "%c:%s", method ? 'm' : 'f', name);
    return key;
}
static Value member_read(Compiler *c, Value base, const char *name) {
    const AST *type = member_type(c, base, name, 0);
    if (!type)
        return (Value){0};
    const ObjectClass *class = value_class(c, base);
    nonnull(c, base);
    char *key = member_key(name, objects_field(class, name) == NULL);
    Value value = convert(c, fetch(c, base, key), moarvm_type_from_ast(type));
    value.type_info = type;
    free(key);
    return value;
}
static void member_write(Compiler *c, Value base, const char *name, Value value, int method) {
    nonnull(c, base);
    char *key = member_key(name, method);
    bind(c, base, key, value);
    free(key);
}
static Value new_object(Compiler *c, AST *n) {
    const ObjectClass *class = objects_find(c->model->objects, n->label);
    Value object = hash(c);
    object.type = MV_OBJECT;
    object.type_info = n->children[0];
    bind(c, object, "@class", integer(c, (int64_t)(class - c->model->objects->classes)));
    for (size_t i = 0; i < class->field_count; i++) {
        char *key = member_key(class->fields[i].name, 0);
        bind(c, object, key, default_value(c, moarvm_type_from_ast(class->fields[i].type)));
        free(key);
    }
    return object;
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
        if (target.var->is_function)
            fail(c, "cannot assign to local function: %s", lhs->label);
        if (!strcmp(lhs->label, "this"))
            fail(c, "cannot reassign this");
        dst.type = target.var->type;
        dst.type_info = target.var->type_info;
        target_type = target.var->type_info;
    } else if (lhs->type == NODE_MEMBER) {
        base = expr(c, lhs->children[0]);
        target_type = member_type(c, base, lhs->label, 1);
        if (!target_type)
            return dst;
        dst.type = moarvm_type_from_ast(target_type);
    } else if (lhs->type == NODE_INDEX) {
        base = expr(c, lhs->children[0]);
        index = convert(c, expr(c, lhs->children[1]->children[0]), MV_INT);
        if (base.type != MV_INT_ARRAY && base.type != MV_STRING_ARRAY &&
            base.type != MV_OBJECT_ARRAY && base.type != MV_DYNAMIC)
            fail(c, "array assignment requires a mutable array");
        dst.type = base.type == MV_STRING_ARRAY   ? MV_STRING
                   : base.type == MV_OBJECT_ARRAY ? MV_OBJECT
                                                  : MV_INT;
        if (base.type == MV_OBJECT_ARRAY && base.type_info)
            target_type = base.type_info->children[0];
    } else {
        fail(c, "assignment target must be a variable or array element");
        return dst;
    }
    if (strcmp(node->label, "=")) {
        if (lhs->type == NODE_INDEX)
            previous = index_read(c, base, index);
        else if (lhs->type == NODE_MEMBER)
            previous = member_read(c, base, lhs->label);
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
    else if (lhs->type == NODE_MEMBER)
        member_write(c, base, lhs->label, rhs, 0);
    else
        emit(c,
             dst.type == MV_STRING   ? OP_BINDPOS_S
             : dst.type == MV_OBJECT ? OP_BINDPOS_O
                                     : OP_BINDPOS_I,
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
    if (a.type == MV_INT_ARRAY || a.type == MV_STRING_ARRAY || a.type == MV_OBJECT_ARRAY)
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
    if (callee->type != NODE_IDENTIFIER)
        return invoke(c, expr(c, callee), args, "expression");
    const char *name = callee->label;
    if (moarvm_resolve_variable(c->model, c->fn, name, NULL, NULL))
        return invoke(c, load_binding(c, binding(c, name)), args, name);
    size_t n = (size_t)args->nchildren;
    const ObjectClass *class = objects_find(c->model->objects, name);
    if (class && !class->constructor && class->field_count && !n) {
        size_t len = strlen(name) + 12;
        char *default_name = malloc(len);
        snprintf(default_name, len, "$default$%s$", name);
        const MoarVMFunctionModel *def = moarvm_find_function(c->model, default_name);
        free(default_name);
        return user_call(c, def, args);
    }
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
    case NODE_OBJECT_NEW:
        return new_object(c, n);
    case NODE_MEMBER: {
        Value base = expr(c, n->children[0]);
        return member_read(c, base, n->label);
    }
    case NODE_OBJECT_IS:
        return object_is(c, expr(c, n->children[0]), n->label);
    case NODE_OBJECT_CAST: {
        Value value = expr(c, n->children[0]);
        value.type = MV_OBJECT;
        value.type_info = n->children[1];
        return value;
    }
    case NODE_METHOD_BIND: {
        Value base = expr(c, n->children[0]), value = expr(c, n->children[1]);
        const AST *type = member_type(c, base, n->label, 0);
        if (type)
            value = convert_type(c, value, type);
        member_write(c, base, n->label, value, 1);
        return value;
    }
    case NODE_LITERAL:
        return literal(c, n->label);
    case NODE_IDENTIFIER: {
        if (moarvm_resolve_variable(c->model, c->fn, n->label, NULL, NULL))
            return load_binding(c, binding(c, n->label));
        const MoarVMFunctionModel *f = moarvm_find_function(c->model, n->label);
        if (f)
            return function_value(c, f);
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
    v = type == MV_FUNCTION || type == MV_OBJECT || type == MV_OBJECT_ARRAY
            ? convert_type(c, v, c->fn->return_type_info)
            : convert(c, v, type);
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
    f.outer_index = c->fn && c->fn->parent_index >= 0 ? (uint16_t)c->frame_ids[c->fn->parent_index]
                                                      : (uint16_t)c->image->frame_count;
    if (c->fn && c->fn->var_count) {
        f.num_lexicals = (uint32_t)c->fn->var_count;
        f.lexical_types = malloc(f.num_lexicals * sizeof(*f.lexical_types));
        f.lexical_name_indices = malloc(f.num_lexicals * sizeof(*f.lexical_name_indices));
        if (!f.lexical_types || !f.lexical_name_indices) {
            moarvm_frame_clear(&f);
            fail(c, "lexical allocation failed");
            return;
        }
        for (uint32_t i = 0; i < f.num_lexicals; i++) {
            f.lexical_types[i] = regtype(c->fn->vars[i].type);
            f.lexical_name_indices[i] = string(c, c->fn->vars[i].name);
        }
    }
    if (moarvm_frame_set_local_types(&f, c->register_types, (uint32_t)c->registers) != 0 ||
        moarvm_image_add_frame(c->image, &f) == UINT32_MAX)
        fail(c, "frame allocation failed");
    moarvm_frame_clear(&f);
}
static void compile_function(Compiler *c, const MoarVMFunctionModel *f) {
    c->fn = f;
    begin_frame(c);
    if (f->var_count > UINT16_MAX) {
        fail(c, "too many lexical variables");
        return;
    }
    if (f->param_count > INT16_MAX) {
        fail(c, "too many parameters");
        return;
    }
    E2(c, CHECKARITY, f->param_count, f->param_count);
    for (size_t i = 0; i < f->param_count; i++) {
        Value v = temp(c, f->vars[i].type);
        emit(c,
             v.type == MV_INT      ? OP_PARAM_RP_I
             : v.type == MV_STRING ? OP_PARAM_RP_S
                                   : OP_PARAM_RP_O,
             (uint64_t)v.reg, (uint64_t)i);
        E3(c, BINDLEX, i, 0, v.reg);
    }
    E0(c, PARAMNAMESUSED);
    for (size_t i = f->param_count; i < f->var_count; i++) {
        if (f->vars[i].is_function)
            continue;
        Value d = default_value(c, f->vars[i].type);
        E3(c, BINDLEX, i, 0, d.reg);
    }
    for (size_t i = 0; i < c->model->function_count; i++) {
        const MoarVMFunctionModel *child = &c->model->functions[i];
        if (child->parent_index != (int)(f - c->model->functions))
            continue;
        Value code = function_value(c, child), closure = temp(c, MV_FUNCTION);
        E2(c, TAKECLOSURE, closure.reg, code.reg);
        store_binding(c, binding(c, child->signature->label), closure);
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
            const MoarVMVariableModel *result = moarvm_find_variable(f, "result");
            Value value = f->return_type == MV_VOID     ? (Value){.type = MV_VOID}
                          : result && !result->is_param ? load_binding(c, binding(c, "result"))
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
static Value object_array(Compiler *c) {
    Value type = boot(c, OP_BOOTARRAY);
    return unary(c, OP_CREATE, MV_DYNAMIC, type);
}
static void array_put(Compiler *c, Value a, int index, Value value) {
    Value at = integer(c, index), object = convert(c, value, MV_DYNAMIC);
    E3(c, BINDPOS_O, a.reg, at.reg, object.reg);
}
static Value array_get(Compiler *c, Value a, int index) {
    return binary(c, OP_ATPOS_O, MV_DYNAMIC, a, integer(c, index));
}
static void hll_bind(Compiler *c, const char *name, Value value) {
    Value key = textval(c, name), result = temp(c, MV_DYNAMIC);
    value = convert(c, value, MV_DYNAMIC);
    E3(c, BINDCURHLLSYM, result.reg, key.reg, value.reg);
}
static Value hll_get(Compiler *c, const char *name) {
    return unary(c, OP_GETCURHLLSYM, MV_DYNAMIC, textval(c, name));
}
static char ffi_type(MoarVMType type) {
    return type == MV_INT            ? 'i'
           : type == MV_STRING       ? 's'
           : type == MV_INT_ARRAY    ? 'I'
           : type == MV_STRING_ARRAY ? 'S'
           : type == MV_VOID         ? 'v'
                                     : 0;
}
static void compile_exports(Compiler *c) {
    begin_frame(c);
    c->fn = NULL;
    E2(c, CHECKARITY, 0, 0);
    E0(c, PARAMNAMESUSED);
    Value exports = object_array(c);
    int count = 0;
    for (size_t i = 0; i < c->model->function_count; i++) {
        const MoarVMFunctionModel *f = &c->model->functions[i];
        if (!f->has_body || f->parent_index >= 0 || !strncmp(f->source_name, "__spo_", 6))
            continue;
        char *signature = calloc(f->param_count + 4, 1);
        if (!signature) {
            fail(c, "out of memory building export table");
            break;
        }
        signature[0] = ffi_type(f->return_type);
        signature[1] = '(';
        int supported = signature[0] != 0;
        for (size_t j = 0; j < f->param_count; j++) {
            signature[j + 2] = ffi_type(f->vars[j].type);
            if (!signature[j + 2])
                supported = 0;
        }
        signature[f->param_count + 2] = ')';
        if (supported) {
            Value entry = object_array(c);
            array_put(c, entry, SPO_EXPORT_NAME, textval(c, f->source_name));
            array_put(c, entry, SPO_EXPORT_CODE, function_value(c, f));
            array_put(c, entry, SPO_EXPORT_SIGNATURE, textval(c, signature));
            array_put(c, exports, count++, entry);
        }
        free(signature);
    }
    if (!count)
        fail(c,
             "library has no exportable functions: use explicit int/string/array/void signatures");
    hll_bind(c, SPO_FFI_EXPORTS, exports);
    hll_bind(c, SPO_FFI_VERSION, integer(c, SPO_FFI_ABI_VERSION));
    E0(c, RETURN);
    end_frame(c, "__spo_library_init");
    c->image->load_frame_index = (uint32_t)c->image->frame_count;
    c->image->main_frame_index = c->image->load_frame_index;
}
int moarvm_generate_ffi_runtime_image(MoarVMImage *image, char **error) {
    Compiler c = {0};
    c.image = image;
    moarvm_image_init(image);
    image->hll_name_string_index = string(&c, SPO_FFI_HLL);
    begin_frame(&c);
    E2(&c, CHECKARITY, 0, 0);
    E0(&c, PARAMNAMESUSED);
    Value exception = temp(&c, MV_DYNAMIC);
    E1(&c, EXCEPTION, exception.reg);
    Value message = unary(&c, OP_GETEXMESSAGE, MV_STRING, exception);
    Value request = hll_get(&c, SPO_FFI_REQUEST);
    array_put(&c, request, SPO_FFI_ERROR, message);
    Value nil = boot(&c, OP_NULL);
    E1(&c, RETURN_O, nil.reg);
    end_frame(&c, "__spo_ffi_catch");
    for (int action = SPO_RUNTIME_LOAD; action <= SPO_RUNTIME_CALL; action++) {
        begin_frame(&c);
        E2(&c, CHECKARITY, 0, 0);
        E0(&c, PARAMNAMESUSED);
        Value handler = frame_code(&c, SPO_RUNTIME_CATCH);
        uint32_t start = (uint32_t)image->bytecode_size - c.frame_start;
        request = hll_get(&c, SPO_FFI_REQUEST);
        if (action == SPO_RUNTIME_LOAD) {
            Value path = convert(&c, array_get(&c, request, SPO_FFI_CODE), MV_STRING);
            Value loaded = temp(&c, MV_STRING);
            E2(&c, LOADBYTECODE, loaded.reg, path.reg);
        } else {
            Value code = array_get(&c, request, SPO_FFI_CODE),
                  args = array_get(&c, request, SPO_FFI_ARGS);
            Value is_void = convert(&c, array_get(&c, request, SPO_FFI_VOID), MV_INT);
            uint8_t flags[] = {1, 65};
            uint32_t cs = moarvm_image_add_callsite(image, flags, 2),
                     name = string(&c, "boot-code");
            uint32_t void_branch = jump(&c, OP_IF_I, is_void);
            Value result = temp(&c, MV_DYNAMIC);
            E3(&c, DISPATCH_O, result.reg, name, cs);
            u16(&c, code.reg);
            u16(&c, args.reg);
            array_put(&c, request, SPO_FFI_RESULT, result);
            uint32_t done = jump(&c, OP_GOTO, (Value){0});
            here(&c, void_branch);
            E2(&c, DISPATCH_V, name, cs);
            u16(&c, code.reg);
            u16(&c, args.reg);
            here(&c, done);
        }
        uint32_t end = (uint32_t)image->bytecode_size - c.frame_start;
        E0(&c, RETURN);
        end_frame(&c, action == SPO_RUNTIME_LOAD ? "__spo_ffi_load" : "__spo_ffi_call");
        MoarVMFrame *f = &image->frames[image->frame_count - 1];
        f->num_handlers = 1;
        f->handlers = malloc(sizeof(*f->handlers));
        if (!f->handlers) {
            f->num_handlers = 0;
            fail(&c, "out of memory building FFI handlers");
            break;
        }
        *f->handlers = (MoarVMHandler){start, end, 1, 2, handler.reg, end};
    }
    image->main_frame_index = SPO_RUNTIME_LOAD + 1;
    free(c.register_types);
    if (error)
        *error = c.error;
    else
        free(c.error);
    if (c.error) {
        moarvm_image_free(image);
        return -1;
    }
    return 0;
}
static int generate_program(const MoarVMProgramModel *model, MoarVMImage *image, char **error,
                            int library) {

    Compiler c = {0};
    c.model = model;
    c.image = image;
    moarvm_image_init(image);
    if (error)
        *error = NULL;
    const MoarVMFunctionModel *main = moarvm_find_function(model, "main");
    if (!library && (!main || !main->has_body || main->param_count)) {
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
    if (!c.error && library)
        compile_exports(&c);
    if (!c.error && !library) {
        begin_frame(&c);
        c.fn = NULL;
        call_frame(&c, c.frame_ids[main - model->functions], main->return_type, NULL, 0);
        E0(&c, RETURN);
        end_frame(&c, "__spo_start");
        image->main_frame_index = (uint32_t)image->frame_count;
    }
    free(c.frame_ids);
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
int moarvm_generate_image(const MoarVMProgramModel *m, MoarVMImage *image, char **error) {
    return generate_program(m, image, error, 0);
}
int moarvm_generate_library_image(const MoarVMProgramModel *m, MoarVMImage *image, char **error) {
    return generate_program(m, image, error, 1);
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
