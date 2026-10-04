#include "objects.h"
#include "moarvm_model.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static AST *node(NodeType kind, const char *label, AST *a, AST *b) {
    AST *n = ast_new(kind, label);
    ast_append(n, a);
    ast_append(n, b);
    return n;
}
static AST *copy(const AST *n) {
    return ast_copy((AST *)n);
}
static AST *id(const char *s) {
    return ast_new(NODE_IDENTIFIER, s);
}
static AST *otype(const char *s) {
    return node(NODE_TYPE, "object", ast_new(NODE_TYPE, s), NULL);
}
static AST *type_of(const AST *n) {
    for (int i = 0; i < n->nchildren; i++)
        if (n->children[i]->type == NODE_TYPE)
            return n->children[i];
    return NULL;
}
static AST *params(const AST *sig) {
    for (int i = 0; i < sig->nchildren; i++)
        if (sig->children[i]->type == NODE_PARAM_LIST)
            return sig->children[i];
    return NULL;
}
static AST *ftype(const AST *sig) {
    AST *t = node(NODE_TYPE, "function", copy(type_of(sig)), NULL), *p = params(sig);
    for (int i = 0; i < p->nchildren; i++) {
        AST *param = type_of(p->children[i]);
        ast_append(t, param ? copy(param) : ast_new(NODE_TYPE, "dynamic"));
    }
    return t;
}
static void fail(ObjectProgram *p, const char *fmt, ...) {
    if (p->error)
        return;
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    p->error = strdup(message);
}
const ObjectClass *objects_find(const ObjectProgram *p, const char *name) {
    if (p)
        for (size_t i = 0; i < p->count; i++)
            if (!strcmp(p->classes[i].name, name))
                return &p->classes[i];
    return NULL;
}
const ObjectField *objects_field(const ObjectClass *c, const char *name) {
    if (c)
        for (size_t i = 0; i < c->field_count; i++)
            if (!strcmp(c->fields[i].name, name))
                return &c->fields[i];
    return NULL;
}
const ObjectMethod *objects_method(const ObjectClass *c, const char *name) {
    if (c)
        for (size_t i = 0; i < c->method_count; i++)
            if (!strcmp(c->methods[i].name, name))
                return &c->methods[i];
    return NULL;
}
int objects_subtype(const ObjectProgram *p, const char *derived, const char *base) {
    const ObjectClass *c = objects_find(p, derived);
    while (c) {
        if (!strcmp(c->name, base))
            return 1;
        c = c->parent < 0 ? NULL : &p->classes[c->parent];
    }
    return 0;
}
static int valid_type(ObjectProgram *p, const AST *t, int result) {
    MoarVMType kind = moarvm_type_from_ast(t);
    if (!t || kind == MV_DYNAMIC || kind == MV_INVALID || (kind == MV_VOID && !result))
        return 0;
    if (kind == MV_OBJECT)
        return objects_find(p, t->children[0]->label) != NULL;
    if (kind == MV_FUNCTION) {
        for (int i = 0; i < t->nchildren; i++)
            if (!valid_type(p, t->children[i], i == 0))
                return 0;
    }
    if (kind == MV_OBJECT_ARRAY)
        return valid_type(p, t->children[0], 0);
    return 1;
}
static void add_member(ObjectProgram *p, ObjectClass *c, const AST *n, int owner) {
    if (n->type == NODE_PARAM_LIST) {
        for (int i = 0; i < n->nchildren; i++)
            add_member(p, c, n->children[i], owner);
        return;
    }
    if (n->type == NODE_VAR_DECL) {
        if (objects_field(c, n->label) || objects_method(c, n->label)) {
            fail(p, "duplicate/inherited member: %s.%s", c->name, n->label);
            return;
        }
        const AST *t = type_of(n);
        if (!valid_type(p, t, 0)) {
            fail(p, "unsupported field type: %s.%s", c->name, n->label);
            return;
        }
        const AST *init = NULL;
        for (int i = 0; i < n->nchildren; i++)
            if (n->children[i]->type != NODE_TYPE)
                init = n->children[i];
        c->fields = realloc(c->fields, (c->field_count + 1) * sizeof(*c->fields));
        c->fields[c->field_count++] = (ObjectField){n->label, t, init};
    } else if (n->type == NODE_FUNCTION) {
        if (!strcmp(n->label, c->name)) {
            if (c->constructor)
                fail(p, "duplicate constructor: %s", c->name);
            if (moarvm_type_from_ast(type_of(n->children[0])) != MV_VOID)
                fail(p, "constructor must not declare a return type: %s", c->name);
            AST *type = ftype(n->children[0]);
            if (!valid_type(p, type, 0))
                fail(p, "constructor requires explicit supported signature: %s", c->name);
            ast_free(type);
            c->constructor = n;
            return;
        }
        if (objects_field(c, n->label)) {
            fail(p, "field/method conflict: %s.%s", c->name, n->label);
            return;
        }
        AST *type = ftype(n->children[0]);
        if (!valid_type(p, type, 0))
            fail(p, "method requires explicit supported signature: %s.%s", c->name, n->label);
        ObjectMethod *old = (ObjectMethod *)objects_method(c, n->label);
        if (old) {
            if (old->owner == owner)
                fail(p, "duplicate method: %s.%s", c->name, n->label);
            if (!moarvm_types_equal(old->type, type))
                fail(p, "incompatible method override: %s.%s", c->name, n->label);
            ast_free(old->type);
            *old = (ObjectMethod){n->label, n, type, owner};
        } else {
            c->methods = realloc(c->methods, (c->method_count + 1) * sizeof(*c->methods));
            c->methods[c->method_count++] = (ObjectMethod){n->label, n, type, owner};
        }
    }
}
static void layout(ObjectProgram *p, int index) {
    ObjectClass *c = &p->classes[index];
    if (c->state == 2 || p->error)
        return;
    if (c->state == 1) {
        fail(p, "inheritance cycle: %s", c->name);
        return;
    }
    c->state = 1;
    const char *parent = c->definition->children[0]->label;
    if (*parent) {
        const ObjectClass *base = objects_find(p, parent);
        if (!base) {
            fail(p, "unknown parent class: %s", parent);
            return;
        }
        c->parent = (int)(base - p->classes);
        layout(p, c->parent);
        if (p->error)
            return;
        if (base->field_count) {
            c->fields = malloc(base->field_count * sizeof(*c->fields));
            memcpy(c->fields, base->fields, base->field_count * sizeof(*c->fields));
            c->field_count = base->field_count;
        }
        for (size_t j = 0; j < base->method_count; j++) {
            c->methods = realloc(c->methods, (c->method_count + 1) * sizeof(*c->methods));
            c->methods[c->method_count] = base->methods[j];
            c->methods[c->method_count++].type = copy(base->methods[j].type);
        }
    }
    const AST *members = c->definition->children[1];
    for (int i = 0; i < members->nchildren; i++)
        add_member(p, c, members->children[i], index);
    c->state = 2;
}
static AST *member(AST *base, const char *name) {
    return node(NODE_MEMBER, name, base, NULL);
}
static AST *assign(AST *lhs, AST *rhs) {
    return node(NODE_EXPR_STMT, "expression", node(NODE_BINARY, "=", lhs, rhs), NULL);
}
static AST *decl(const char *name, AST *type, AST *init) {
    return node(NODE_VAR_DECL, name, init, type);
}
static void rename_node(AST *n, const char *name) {
    free(n->label);
    n->label = strdup(name);
}
static char *hidden(const char *role, const char *owner, const char *name) {
    size_t length = strlen(role) + strlen(owner) + strlen(name) + 4;
    char *s = malloc(length);
    snprintf(s, length, "$%s$%s$%s", role, owner, name);
    return s;
}
static void rewrite_super(ObjectProgram *p, AST *n, const ObjectClass *owner, int initializer) {
    if (!n || p->error)
        return;
    if (n->type == NODE_MEMBER && n->children[0]->type == NODE_IDENTIFIER &&
        !strcmp(n->children[0]->label, "super")) {
        const ObjectClass *base = owner->parent < 0 ? NULL : &p->classes[owner->parent];
        const ObjectMethod *method = objects_method(base, n->label);
        if (!method) {
            fail(p, "unknown super method: %s.%s", owner->name, n->label);
            return;
        }
        char *name = hidden("method", p->classes[method->owner].name, n->label);
        ast_free(n->children[0]);
        free(n->children);
        n->children = NULL;
        n->nchildren = 0;
        n->type = NODE_IDENTIFIER;
        rename_node(n, name);
        free(name);
        return;
    }
    if (n->type == NODE_CALL && n->children[0]->type == NODE_IDENTIFIER &&
        !strcmp(n->children[0]->label, "super")) {
        if (!initializer) {
            fail(p, "super constructor call outside constructor: %s", owner->name);
            return;
        }
        if (owner->parent < 0) {
            fail(p, "super constructor requires a parent: %s", owner->name);
            return;
        }
        char *name = hidden("init", p->classes[owner->parent].name, "");
        rename_node(n->children[0], name);
        free(name);
    }
    for (int i = 0; i < n->nchildren; i++)
        rewrite_super(p, n->children[i], owner, initializer);
}
static AST *constructor_params(const ObjectClass *c) {
    if (c->constructor)
        return copy(params(c->constructor->children[0]));
    AST *p = ast_new(NODE_PARAM_LIST, "params");
    for (size_t i = 0; i < c->field_count; i++)
        ast_append(p, node(NODE_PARAM, c->fields[i].name, copy(c->fields[i].type), NULL));
    return p;
}
static AST *make_constructor(ObjectProgram *p, ObjectClass *c, int defaults) {
    AST *body = ast_new(NODE_BLOCK, "object construction");
    AST *signature = node(NODE_PARAM_LIST, c->name, otype(c->name),
                          defaults ? ast_new(NODE_PARAM_LIST, "params") : constructor_params(c));
    AST *constructor_args = params(signature);
    for (int i = 0; i < constructor_args->nchildren; i++) {
        char *arg = hidden("arg", c->name, constructor_args->children[i]->label);
        rename_node(constructor_args->children[i], arg);
        free(arg);
    }
    char *default_name = hidden("default", c->name, "");
    if (defaults)
        rename_node(signature, default_name);
    AST *function = node(NODE_FUNCTION, signature->label, signature, body);
    free(default_name);
    ast_append(body,
               decl("this", otype(c->name), node(NODE_OBJECT_NEW, c->name, otype(c->name), NULL)));
    const ObjectClass *owner = c;
    while (owner) {
        for (size_t i = 0; i < owner->method_count; i++) {
            const ObjectMethod *m = &owner->methods[i];
            if (m->owner != (int)(owner - p->classes))
                continue;
            AST *method = copy(m->definition);
            char *name = hidden("method", owner->name, m->name);
            rename_node(method, name);
            rename_node(method->children[0], name);
            free(name);
            rewrite_super(p, method->children[1], owner, 0);
            ast_append(body, method);
        }
        AST *init_body;
        if (owner->constructor)
            init_body = copy(owner->constructor->children[1]);
        else {
            init_body = ast_new(NODE_BLOCK, "positional initializer");
            for (size_t i = 0; i < owner->field_count; i++)
                ast_append(init_body, assign(member(id("this"), owner->fields[i].name),
                                             id(owner->fields[i].name)));
        }
        rewrite_super(p, init_body, owner, 1);
        char *name = hidden("init", owner->name, "");
        AST *sig =
            node(NODE_PARAM_LIST, name, ast_new(NODE_TYPE, "void"), constructor_params(owner));
        ast_append(body, node(NODE_FUNCTION, name, sig, init_body));
        free(name);
        owner = owner->parent < 0 ? NULL : &p->classes[owner->parent];
    }
    for (size_t i = 0; i < c->method_count; i++) {
        const ObjectMethod *m = &c->methods[i];
        char *name = hidden("method", p->classes[m->owner].name, m->name);
        ast_append(body, node(NODE_EXPR_STMT, "bind method",
                              node(NODE_METHOD_BIND, m->name, id("this"), id(name)), NULL));
        free(name);
    }
    for (size_t i = 0; i < c->field_count; i++)
        if (c->fields[i].initializer)
            ast_append(body, assign(member(id("this"), c->fields[i].name),
                                    copy(c->fields[i].initializer)));
    if (!defaults) {
        char *name = hidden("init", c->name, "");
        AST *args = ast_new(NODE_PARAM_LIST, "args"), *formal = params(signature);
        for (int i = 0; i < formal->nchildren; i++)
            ast_append(args, id(formal->children[i]->label));
        ast_append(body, node(NODE_EXPR_STMT, "initialize", node(NODE_CALL, "call", id(name), args),
                              NULL));
        free(name);
    }
    ast_append(body, node(NODE_RETURN, "return", id("this"), NULL));
    return function;
}

typedef struct Scope {
    char **names;
    size_t count;
    struct Scope *outer;
} Scope;
static int known(Scope *s, const char *name) {
    for (; s; s = s->outer)
        for (size_t i = 0; i < s->count; i++)
            if (!strcmp(s->names[i], name))
                return 1;
    return 0;
}
static void remember(Scope *s, const char *name) {
    s->names = realloc(s->names, (s->count + 1) * sizeof(*s->names));
    s->names[s->count++] = strdup(name);
}
static void collect(Scope *s, AST *n) {
    if (n->type == NODE_FUNCTION) {
        remember(s, n->label);
        return;
    }
    if (n->type == NODE_VAR_DECL || n->type == NODE_PARAM)
        remember(s, n->label);
    if (n->type == NODE_TYPE)
        return;
    for (int i = 0; i < n->nchildren; i++)
        collect(s, n->children[i]);
}
static void lower(ObjectProgram *, AST **, Scope *);
static void lower_function(ObjectProgram *p, AST *fn, Scope *outer) {
    if (fn->nchildren < 2)
        return;
    Scope scope = {.outer = outer};
    collect(&scope, params(fn->children[0]));
    collect(&scope, fn->children[1]);
    lower(p, &fn->children[1], &scope);
    for (size_t i = 0; i < scope.count; i++)
        free(scope.names[i]);
    free(scope.names);
}
static void lower(ObjectProgram *p, AST **slot, Scope *scope) {
    AST *n = *slot;
    if (n->type == NODE_FUNCTION) {
        lower_function(p, n, scope);
        return;
    }
    if (n->type == NODE_TYPE || n->type == NODE_CLASS)
        return;
    if (n->type == NODE_MATCH) {
        AST *block = ast_new(NODE_BLOCK, "lowered match");
        char temp[64];
        snprintf(temp, sizeof(temp), "$match$%u", p->temporary++);
        ast_append(block, decl(temp, ast_new(NODE_TYPE, "dynamic"), copy(n->children[0])));
        remember(scope, temp);
        AST *chain = NULL;
        AST **tail = &chain;
        AST *cases = n->children[1];
        for (int i = 0; i < cases->nchildren; i++) {
            AST *arm = cases->children[i], *names = arm->children[0];
            const ObjectClass *c = objects_find(p, arm->label);
            if (!c) {
                fail(p, "unknown match class: %s", arm->label);
                break;
            }
            if ((size_t)names->nchildren != c->field_count) {
                fail(p, "match %s expects %zu fields, got %d", c->name, c->field_count,
                     names->nchildren);
                break;
            }
            for (int j = 0; j < i; j++)
                if (objects_subtype(p, c->name, cases->children[j]->label))
                    fail(p, "unreachable match case: %s", c->name);
            AST *arm_body = ast_new(NODE_BLOCK, "bindings");
            for (int j = 0; j < names->nchildren; j++) {
                const char *name = names->children[j]->label;
                if (!strcmp(name, "_"))
                    continue;
                for (int k = 0; k < j; k++)
                    if (!strcmp(name, names->children[k]->label))
                        fail(p, "duplicate match binding: %s", name);
                AST *value = member(node(NODE_OBJECT_CAST, c->name, id(temp), otype(c->name)),
                                    c->fields[j].name);
                if (known(scope, name))
                    ast_append(arm_body, assign(id(name), value));
                else {
                    ast_append(arm_body, decl(name, copy(c->fields[j].type), value));
                    remember(scope, name);
                }
            }
            ast_append(arm_body, copy(arm->children[1]));
            AST *conditional = node(NODE_IF, "match case",
                                    node(NODE_OBJECT_IS, c->name, id(temp), NULL), arm_body);
            ast_free(*tail);
            *tail = conditional;
            ast_append(conditional, ast_new(NODE_BLOCK, "unmatched"));
            tail = &conditional->children[2];
        }
        if (chain)
            ast_append(block, chain);
        ast_free(n);
        *slot = block;
        n = block;
    }
    for (int i = 0; i < n->nchildren; i++)
        lower(p, &n->children[i], scope);
}
int objects_prepare(ObjectProgram *p, AST **trees, int count) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < count; i++)
        for (int j = 0; j < trees[i]->nchildren; j++) {
            AST *n = trees[i]->children[j];
            if (n->type != NODE_CLASS)
                continue;
            if (objects_find(p, n->label)) {
                fail(p, "duplicate class: %s", n->label);
                continue;
            }
            p->classes = realloc(p->classes, (p->count + 1) * sizeof(*p->classes));
            p->classes[p->count++] = (ObjectClass){.definition = n, .name = n->label, .parent = -1};
        }
    for (size_t i = 0; i < p->count; i++)
        layout(p, (int)i);
    if (p->error)
        return -1;
    for (int i = 0; i < count; i++) {
        int original_count = trees[i]->nchildren;
        for (int j = 0; j < original_count; j++) {
            AST *n = trees[i]->children[j];
            if (n->type == NODE_FUNCTION && objects_find(p, n->label))
                fail(p, "function conflicts with class: %s", n->label);
            if (n->type == NODE_CLASS) {
                ObjectClass *c = (ObjectClass *)objects_find(p, n->label);
                ast_append(trees[i], make_constructor(p, c, 0));
                if (!c->constructor && c->field_count)
                    ast_append(trees[i], make_constructor(p, c, 1));
            }
        }
        for (int j = 0; j < trees[i]->nchildren; j++)
            if (trees[i]->children[j]->type == NODE_FUNCTION)
                lower_function(p, trees[i]->children[j], NULL);
    }
    return p->error ? -1 : 0;
}
void objects_free(ObjectProgram *p) {
    for (size_t i = 0; i < p->count; i++) {
        for (size_t j = 0; j < p->classes[i].method_count; j++)
            ast_free(p->classes[i].methods[j].type);
        free(p->classes[i].methods);
        free(p->classes[i].fields);
    }
    free(p->classes);
    free(p->error);
    memset(p, 0, sizeof(*p));
}
