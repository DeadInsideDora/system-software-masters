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
    if (!strcmp(n->label, "array")) {
        MoarVMType e = moarvm_type_from_ast(n->nchildren ? n->children[0] : NULL);
        return e == MV_INT      ? MV_INT_ARRAY
               : e == MV_STRING ? MV_STRING_ARRAY
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
                           "void", "invalid"};
    return t <= MV_INVALID ? names[t] : "invalid";
}
int moarvm_types_equal(const AST *a, const AST *b) {
    MoarVMType x = moarvm_type_from_ast(a), y = moarvm_type_from_ast(b);
    return x != MV_INVALID && x == y;
}
void moarvm_write_type(FILE *out, const AST *type) {
    fputs(moarvm_type_name(moarvm_type_from_ast(type)), out);
}
static void fail(MoarVMProgramModel *m, const char *message, const char *name) {
    if (m->error)
        return;
    size_t size = strlen(message) + strlen(name ? name : "") + 1;
    m->error = malloc(size);
    snprintf(m->error, size, "%s%s", message, name ? name : "");
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
static MoarVMVariableModel *add_var(MoarVMProgramModel *m, MoarVMFunctionModel *f, const char *name,
                                    const AST *type_info, int declared, int param) {
    MoarVMVariableModel *v = (MoarVMVariableModel *)moarvm_find_variable(f, name);
    if (v) {
        if (declared && v->explicit_decl)
            fail(m, "duplicate variable: ", name);
        return v;
    }
    MoarVMType type = moarvm_type_from_ast(type_info);
    if (type == MV_VOID || type == MV_INVALID) {
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
        if (!moarvm_find_variable(f, name))
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
int moarvm_build_program_model(AnalysisResult **analyses, int count,
                               MoarVMProgramModel *m) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < count && !m->error; i++) {
        AnalysisResult *a = analyses[i];
        if (a->nerrors) {
            fail(m, "CFG: ", a->errors[0]);
            break;
        }
        for (int j = 0; j < a->nfunctions; j++) {
            const FunctionCFG *cfg = a->functions[j];
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
            }
            f->cfg = cfg;
            f->signature = cfg->signature;
            f->has_body = cfg->has_body;
        }
    }
    for (size_t i = 0; i < m->function_count && !m->error; i++) {
        MoarVMFunctionModel *f = &m->functions[i];
        f->return_type_info = type_node(f->signature);
        f->return_type = moarvm_type_from_ast(f->return_type_info);
        if (f->return_type == MV_INVALID) {
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
    }
    free(m->functions);
    free(m->error);
    memset(m, 0, sizeof(*m));
}
int moarvm_write_model(FILE *out, const MoarVMProgramModel *m) {
    for (size_t i = 0; i < m->function_count; i++) {
        const MoarVMFunctionModel *f = &m->functions[i];
        fprintf(out, "%s(", f->source_name);
        for (size_t j = 0; j < f->param_count; j++) {
            if (j) fputs(", ", out);
            fputs(moarvm_type_name(f->vars[j].type), out);
        }
        fprintf(out, ") -> %s [%s]\n", moarvm_type_name(f->return_type),
                f->has_body ? "defined" : "declared");
        for (size_t j = 0; j < f->var_count; j++)
            fprintf(out, "  register[%zu] %s %s : %s\n", j,
                    f->vars[j].is_param ? "parameter" : "variable", f->vars[j].name,
                    moarvm_type_name(f->vars[j].type));
        fprintf(out, "  CFG entry=%d nodes=%d source=%s\n",
                f->cfg->entry ? f->cfg->entry->id : -1, f->cfg->nnodes, f->cfg->source_file);
    }
    return ferror(out) ? -1 : 0;
}
