#include "moarvm_model.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

MoarVMType moarvm_type_from_ast(const AST *n) {
    if (!n || !strcmp(n->label, "dynamic"))
        return MV_DYNAMIC;
    if (!strcmp(n->label, "void"))
        return MV_VOID;
    if (!strcmp(n->label, "string"))
        return MV_STRING;
    if (!strcmp(n->label, "object") && n->nchildren == 1)
        return MV_OBJECT;
    if (!strcmp(n->label, "function")) {
        if (!n->nchildren || moarvm_type_from_ast(n->children[0]) == MV_INVALID)
            return MV_INVALID;
        for (int i = 1; i < n->nchildren; i++) {
            MoarVMType t = moarvm_type_from_ast(n->children[i]);
            if (t == MV_INVALID || t == MV_VOID)
                return MV_INVALID;
        }
        return MV_FUNCTION;
    }
    if (!strcmp(n->label, "array")) {
        MoarVMType e = moarvm_type_from_ast(n->nchildren ? n->children[0] : NULL);
        return e == MV_INT      ? MV_INT_ARRAY
               : e == MV_STRING ? MV_STRING_ARRAY
               : e == MV_OBJECT ? MV_OBJECT_ARRAY
                                : MV_INVALID;
    }
    const char *names[] = {"int", "long", "uint", "ulong", "byte", "char", "bool", NULL};
    for (int i = 0; names[i]; i++)
        if (!strcmp(n->label, names[i]))
            return MV_INT;
    return MV_INVALID;
}
const char *moarvm_type_name(MoarVMType t) {
    const char *names[] = {"dynamic", "int64",    "string", "int[]",    "string[]",
                           "void",    "function", "object", "object[]", "invalid"};
    return t <= MV_INVALID ? names[t] : "invalid";
}
int moarvm_types_equal(const AST *a, const AST *b) {
    MoarVMType x = moarvm_type_from_ast(a), y = moarvm_type_from_ast(b);
    if (x != y || x == MV_INVALID)
        return 0;
    if (x == MV_OBJECT)
        return !strcmp(a->children[0]->label, b->children[0]->label);
    if (x == MV_OBJECT_ARRAY)
        return moarvm_types_equal(a->children[0], b->children[0]);
    if (x != MV_FUNCTION)
        return 1;
    if (a->nchildren != b->nchildren)
        return 0;
    for (int i = 0; i < a->nchildren; i++)
        if (!moarvm_types_equal(a->children[i], b->children[i]))
            return 0;
    return 1;
}
void moarvm_write_type(FILE *out, const AST *type) {
    if (moarvm_type_from_ast(type) == MV_OBJECT) {
        fputs(type->children[0]->label, out);
        return;
    }
    if (moarvm_type_from_ast(type) == MV_OBJECT_ARRAY) {
        moarvm_write_type(out, type->children[0]);
        fputs("[]", out);
        return;
    }
    if (moarvm_type_from_ast(type) != MV_FUNCTION) {
        fputs(moarvm_type_name(moarvm_type_from_ast(type)), out);
        return;
    }
    fputs("fn(", out);
    for (int i = 1; i < type->nchildren; i++) {
        if (i > 1)
            fputs(", ", out);
        moarvm_write_type(out, type->children[i]);
    }
    fputs(") -> ", out);
    moarvm_write_type(out, type->children[0]);
}
static void fail(MoarVMProgramModel *m, const char *message, const char *name) {
    if (m->error)
        return;
    size_t size = strlen(message) + strlen(name ? name : "") + 1;
    m->error = malloc(size);
    snprintf(m->error, size, "%s%s", message, name ? name : "");
}
static int known_type(MoarVMProgramModel *m, const AST *type) {
    if (!type)
        return 1;
    if (moarvm_type_from_ast(type) == MV_OBJECT)
        return objects_find(m->objects, type->children[0]->label) != NULL;
    for (int i = 0; i < type->nchildren; i++)
        if (!known_type(m, type->children[i]))
            return 0;
    return 1;
}
const MoarVMFunctionModel *moarvm_find_function(const MoarVMProgramModel *m, const char *name) {
    for (size_t i = 0; i < m->function_count; i++)
        if (!strcmp(m->functions[i].source_name, name))
            return &m->functions[i];
    return NULL;
}
const MoarVMVariableModel *moarvm_find_variable(const MoarVMFunctionModel *f, const char *name) {
    for (size_t i = 0; i < f->var_count; i++)
        if (!strcmp(f->vars[i].name, name))
            return &f->vars[i];
    return NULL;
}
const MoarVMVariableModel *moarvm_resolve_variable(const MoarVMProgramModel *m,
                                                   const MoarVMFunctionModel *f, const char *name,
                                                   unsigned *depth, size_t *slot) {
    unsigned distance = 0;
    while (f) {
        const MoarVMVariableModel *v = moarvm_find_variable(f, name);
        if (v) {
            if (depth)
                *depth = distance;
            if (slot)
                *slot = (size_t)(v - f->vars);
            return v;
        }
        f = f->parent_index < 0 ? NULL : &m->functions[f->parent_index];
        distance++;
    }
    return NULL;
}
static AST *params(AST *sig) {
    for (int i = 0; i < sig->nchildren; i++)
        if (sig->children[i]->type == NODE_PARAM_LIST)
            return sig->children[i];
    return NULL;
}
static AST *type_node(AST *n) {
    for (int i = 0; i < n->nchildren; i++)
        if (n->children[i]->type == NODE_TYPE)
            return n->children[i];
    return NULL;
}
static AST *copy_type(const AST *type) {
    return type ? ast_copy((AST *)type) : ast_new(NODE_TYPE, "dynamic");
}
static AST *function_type(AST *signature) {
    AST *type = ast_new(NODE_TYPE, "function"), *p = params(signature);
    ast_append(type, copy_type(type_node(signature)));
    for (int i = 0; i < p->nchildren; i++)
        ast_append(type, copy_type(type_node(p->children[i])));
    return type;
}
static MoarVMVariableModel *add_var(MoarVMProgramModel *m, MoarVMFunctionModel *f, const char *name,
                                    const AST *type_info, int declared, int param) {
    MoarVMVariableModel *v = (MoarVMVariableModel *)moarvm_find_variable(f, name);
    if (v) {
        if (declared && v->explicit_decl)
            fail(m, "duplicate variable: ", name);
        return v;
    }
    MoarVMType type = moarvm_type_from_ast(type_info);
    if (type == MV_VOID || type == MV_INVALID || !known_type(m, type_info)) {
        fail(m, "unsupported variable type: ", name);
        return NULL;
    }
    f->vars = realloc(f->vars, (f->var_count + 1) * sizeof(*f->vars));
    v = &f->vars[f->var_count++];
    *v = (MoarVMVariableModel){.name = strdup(name),
                               .type = type,
                               .explicit_decl = declared,
                               .is_param = param,
                               .type_info = type_info};
    return v;
}
static void collect_assignments(MoarVMProgramModel *m, MoarVMFunctionModel *f, AST *n) {
    if (!n || n->type == NODE_TYPE)
        return;
    if (n->type == NODE_BINARY && !strcmp(n->label, "=") &&
        n->children[0]->type == NODE_IDENTIFIER) {
        const char *name = n->children[0]->label;
        if (!moarvm_resolve_variable(m, f, name, NULL, NULL))
            add_var(m, f, name, NULL, 0, 0);
    }
    for (int i = 0; i < n->nchildren; i++)
        collect_assignments(m, f, n->children[i]);
}
static int compatible_type(AST *a, AST *b) {
    return moarvm_type_from_ast(a) == MV_DYNAMIC || moarvm_type_from_ast(b) == MV_DYNAMIC ||
           moarvm_types_equal(a, b);
}
static int compatible_signature(AST *a, AST *b) {
    AST *pa = params(a), *pb = params(b);
    if (pa->nchildren != pb->nchildren || !compatible_type(type_node(a), type_node(b)))
        return 0;
    for (int i = 0; i < pa->nchildren; i++)
        if (!compatible_type(type_node(pa->children[i]), type_node(pb->children[i])))
            return 0;
    return 1;
}
int moarvm_build_program_model(AnalysisResult **analyses, int count, const ObjectProgram *objects,
                               MoarVMProgramModel *m) {
    memset(m, 0, sizeof(*m));
    m->objects = objects;
    for (int i = 0; i < count && !m->error; i++) {
        AnalysisResult *a = analyses[i];
        if (a->nerrors) {
            fail(m, "CFG: ", a->errors[0]);
            break;
        }
        for (int j = 0; j < a->nfunctions; j++) {
            const FunctionCFG *cfg = a->functions[j];
            if (cfg->outer && !strncmp(cfg->signature->label, "__spo_", 6)) {
                fail(m, "reserved local function name: ", cfg->signature->label);
                break;
            }
            MoarVMFunctionModel *f = (MoarVMFunctionModel *)moarvm_find_function(m, cfg->name);
            if (f) {
                if (f->has_body && cfg->has_body) {
                    fail(m, "duplicate function definition: ", cfg->name);
                    break;
                }
                if (!compatible_signature(f->signature, cfg->signature)) {
                    fail(m, "conflicting function signature: ", cfg->name);
                    break;
                }
                if (!cfg->has_body)
                    continue;
            } else {
                m->functions =
                    realloc(m->functions, (m->function_count + 1) * sizeof(*m->functions));
                f = &m->functions[m->function_count++];
                memset(f, 0, sizeof(*f));
                f->source_name = strdup(cfg->name);
                f->parent_index = -1;
            }
            f->cfg = cfg;
            f->signature = cfg->signature;
            f->has_body = cfg->has_body;
        }
    }
    for (size_t i = 0; i < m->function_count && !m->error; i++) {
        MoarVMFunctionModel *f = &m->functions[i];
        if (f->cfg->outer) {
            const MoarVMFunctionModel *parent = moarvm_find_function(m, f->cfg->outer->name);
            f->parent_index = (int)(parent - m->functions);
        }
        f->return_type_info = type_node(f->signature);
        f->return_type = moarvm_type_from_ast(f->return_type_info);
        f->value_type = function_type(f->signature);
        if (f->return_type == MV_INVALID || !known_type(m, f->return_type_info)) {
            fail(m, "unsupported return type: ", f->source_name);
            break;
        }
        AST *p = params(f->signature);
        for (int j = 0; j < p->nchildren; j++)
            add_var(m, f, p->children[j]->label, type_node(p->children[j]), 1, 1);
        f->param_count = f->var_count;
        for (int j = 0; j < f->cfg->nnodes; j++) {
            CFGNode *n = f->cfg->nodes[j];
            if (n->kind == CFG_DECL)
                add_var(m, f, n->operation->label, type_node(n->operation), 1, 0);
        }
    }
    for (size_t i = 0; i < m->function_count && !m->error; i++) {
        MoarVMFunctionModel *f = &m->functions[i];
        if (f->parent_index >= 0) {
            MoarVMVariableModel *v = add_var(m, &m->functions[f->parent_index], f->signature->label,
                                             f->value_type, 1, 0);
            if (v)
                v->is_function = 1;
        }
    }
    for (size_t i = 0; i < m->function_count && !m->error; i++)
        for (int j = 0; j < m->functions[i].cfg->nnodes; j++)
            collect_assignments(m, &m->functions[i], m->functions[i].cfg->nodes[j]->operation);
    return m->error ? -1 : 0;
}
void moarvm_free_program_model(MoarVMProgramModel *m) {
    for (size_t i = 0; i < m->function_count; i++) {
        for (size_t j = 0; j < m->functions[i].var_count; j++)
            free(m->functions[i].vars[j].name);
        free(m->functions[i].vars);
        free(m->functions[i].source_name);
        ast_free(m->functions[i].value_type);
    }
    free(m->functions);
    free(m->error);
    memset(m, 0, sizeof(*m));
}
int moarvm_write_model(FILE *out, const MoarVMProgramModel *m) {
    if (m->objects)
        for (size_t i = 0; i < m->objects->count; i++) {
            const ObjectClass *c = &m->objects->classes[i];
            fprintf(out, "class %s parent=%s tag=%zu representation=VMHash\n", c->name,
                    c->parent < 0 ? "none" : m->objects->classes[c->parent].name, i);
            for (size_t j = 0; j < c->field_count; j++) {
                fprintf(out, "  field %s : ", c->fields[j].name);
                moarvm_write_type(out, c->fields[j].type);
                fputc('\n', out);
            }
            for (size_t j = 0; j < c->method_count; j++) {
                fprintf(out, "  virtual %s -> %s.%s : ", c->methods[j].name,
                        m->objects->classes[c->methods[j].owner].name, c->methods[j].name);
                moarvm_write_type(out, c->methods[j].type);
                fputc('\n', out);
            }
        }
    for (size_t i = 0; i < m->function_count; i++) {
        const MoarVMFunctionModel *f = &m->functions[i];
        fprintf(out, "%s : ", f->source_name);
        moarvm_write_type(out, f->value_type);
        fprintf(out, " [%s] outer=%s\n", f->has_body ? "defined" : "declared",
                f->parent_index < 0 ? "none" : m->functions[f->parent_index].source_name);
        for (size_t j = 0; j < f->var_count; j++) {
            fprintf(out, "  lexical[%zu] %s %s : ", j,
                    f->vars[j].is_param      ? "parameter"
                    : f->vars[j].is_function ? "local function"
                                             : "variable",
                    f->vars[j].name);
            moarvm_write_type(out, f->vars[j].type_info);
            fputc('\n', out);
        }
        if (f->has_body)
            fprintf(out, "  CFG entry=%d nodes=%d source=%s\n", f->cfg->entry->id, f->cfg->nnodes,
                    f->cfg->source_file);
    }
    return ferror(out) ? -1 : 0;
}
