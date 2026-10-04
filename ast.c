#include "ast.h"
#include <stdlib.h>
#include <string.h>

char *strdup_s(const char *s) {
    if (!s)
        return NULL;
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

AST *ast_new(NodeType type, const char *label) {
    AST *a = malloc(sizeof(AST));
    a->type = type;
    a->label = label ? strdup_s(label) : NULL;
    a->children = NULL;
    a->nchildren = 0;
    return a;
}

void ast_append(AST *parent, AST *child) {
    if (!parent || !child)
        return;
    parent->nchildren++;
    parent->children = realloc(parent->children, parent->nchildren * sizeof(AST *));
    parent->children[parent->nchildren - 1] = child;
}

AST *ast_copy(AST *source) {
    if (!source)
        return NULL;
    AST *copy = ast_new(source->type, source->label);
    for (int i = 0; i < source->nchildren; i++) {
        ast_append(copy, ast_copy(source->children[i]));
    }
    return copy;
}

void ast_free(AST *a) {
    if (!a)
        return;
    for (int i = 0; i < a->nchildren; i++)
        ast_free(a->children[i]);
    free(a->children);
    free(a->label);
    free(a);
}

int is_builtin_type(const char *s) {
    if (!s)
        return 0;
    const char *builtins[] = {"bool",  "byte", "int",    "uint", "long",
                              "ulong", "char", "string", NULL};
    for (int i = 0; builtins[i]; ++i) {
        if (strcmp(s, builtins[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

void ast_dot_node(FILE *f, AST *a, int *id) {
    int myid = (*id)++;
    char label[256];
    const char *t = a->label ? a->label : "";

    switch (a->type) {
    case NODE_PROGRAM:
        snprintf(label, sizeof(label), "source source");
        break;
    case NODE_FUNCTION:
        snprintf(label, sizeof(label), "func");
        break;
    case NODE_PARAM_LIST:
        if (strcmp(t, "params") == 0) {
            if (a->nchildren == 0)
                snprintf(label, sizeof(label), "empty params");
            else
                snprintf(label, sizeof(label), "params");
        } else if (strcmp(t, "args") == 0)
            snprintf(label, sizeof(label), "args");
        else if (strcmp(t, "vars") == 0)
            snprintf(label, sizeof(label), "var");
        else
            snprintf(label, sizeof(label), "signature %s", t);
        break;
    case NODE_PARAM:
        snprintf(label, sizeof(label), "param %s", t);
        break;
    case NODE_BLOCK:
        snprintf(label, sizeof(label), "block");
        break;
    case NODE_VAR_DECL:
        snprintf(label, sizeof(label), "var-entry %s", t);
        break;
    case NODE_IF:
        snprintf(label, sizeof(label), "if");
        break;
    case NODE_WHILE:
        snprintf(label, sizeof(label), "while");
        break;
    case NODE_DO_WHILE:
        snprintf(label, sizeof(label), "do-while");
        break;
    case NODE_BREAK:
        snprintf(label, sizeof(label), "break");
        break;
    case NODE_CONTINUE:
        snprintf(label, sizeof(label), "continue");
        break;
    case NODE_RETURN:
        snprintf(label, sizeof(label), "return");
        break;
    case NODE_TYPE:
        snprintf(label, sizeof(label), "type %s", t);
        break;
    case NODE_EXPR_STMT:
        snprintf(label, sizeof(label), "expr-stmt");
        break;
    case NODE_CALL:
        if (t && strcmp(t, "call") == 0) {
            snprintf(label, sizeof(label), "call");
        } else {
            snprintf(label, sizeof(label), "call %s", t);
        }
        break;
    case NODE_INDEX:
        snprintf(label, sizeof(label), "indexer");
        break;
    case NODE_BINARY:
        snprintf(label, sizeof(label), "binary %s", t);
        break;
    case NODE_UNARY:
        snprintf(label, sizeof(label), "unary %s", t);
        break;
    case NODE_LITERAL:
        snprintf(label, sizeof(label), "literal %s", t);
        break;
    case NODE_IDENTIFIER:
        if (strcmp(t, "array") == 0)
            snprintf(label, sizeof(label), "type-array");
        else if (is_builtin_type(t))
            snprintf(label, sizeof(label), "type-builtin %s", t);
        else
            snprintf(label, sizeof(label), "identifier %s", t);
        break;
    default:
        snprintf(label, sizeof(label), "%s", t && *t ? t : "<node>");
        break;
    }

    char esc[512];
    esc[0] = '\0';
    const char *p = label;
    char *q = esc;
    size_t qrem = sizeof(esc) - 1;
    while (*p && qrem) {
        if (*p == '"' || *p == '\\') {
            if (qrem > 2) {
                *q++ = '\\';
                *q++ = *p;
                qrem -= 2;
            } else {
                break;
            }
        } else {
            *q++ = *p;
            qrem--;
        }
        p++;
    }
    *q = '\0';

    fprintf(f, "  node%d [label=\"%s\"];\n", myid, esc);
    for (int i = 0; i < a->nchildren; i++) {
        int childid = *id;
        ast_dot_node(f, a->children[i], id);
        fprintf(f, "  node%d -> node%d;\n", myid, childid);
    }
}

void ast_to_dot(FILE *f, AST *root) {
    if (!root)
        return;
    fprintf(f, "digraph AST {\n");
    int id = 0;
    ast_dot_node(f, root, &id);
    fprintf(f, "}\n");
}
