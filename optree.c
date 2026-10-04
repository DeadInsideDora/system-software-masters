#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "optree.h"
#include "ast.h"

static char *dupstr(const char *s) {
    if (!s)
        return NULL;
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

OpNode *optree_new(OpNodeType type) {
    OpNode *node = malloc(sizeof(OpNode));
    node->type = type;
    node->value = NULL;
    node->op = NULL;
    node->type_info = NULL;
    node->children = NULL;
    node->nchildren = 0;
    return node;
}

OpNode *optree_literal(const char *value) {
    OpNode *node = optree_new(OP_LITERAL);
    node->value = dupstr(value);
    return node;
}

OpNode *optree_identifier(const char *name) {
    OpNode *node = optree_new(OP_IDENTIFIER);
    node->value = dupstr(name);
    return node;
}

OpNode *optree_binary(const char *op, OpNode *left, OpNode *right) {
    OpNode *node = optree_new(OP_BINARY);
    node->op = dupstr(op);
    node->children = malloc(2 * sizeof(OpNode *));
    node->children[0] = left;
    node->children[1] = right;
    node->nchildren = 2;
    return node;
}

OpNode *optree_unary(const char *op, OpNode *operand) {
    OpNode *node = optree_new(OP_UNARY);
    node->op = dupstr(op);
    node->children = malloc(sizeof(OpNode *));
    node->children[0] = operand;
    node->nchildren = 1;
    return node;
}

OpNode *optree_call(const char *func_name, OpNode **args, int nargs) {
    OpNode *node = optree_new(OP_CALL);
    node->op = dupstr(func_name);
    if (nargs > 0) {
        node->children = malloc(nargs * sizeof(OpNode *));
        for (int i = 0; i < nargs; i++) {
            node->children[i] = args[i];
        }
    }
    node->nchildren = nargs;
    return node;
}

OpNode *optree_index(OpNode *array, OpNode *index) {
    OpNode *node = optree_new(OP_INDEX);
    node->children = malloc(2 * sizeof(OpNode *));
    node->children[0] = array;
    node->children[1] = index;
    node->nchildren = 2;
    return node;
}

OpNode *optree_from_ast(AST *a) {
    if (!a)
        return NULL;
    if (a->type == NODE_EXPR_STMT || a->type == NODE_RETURN)
        return a->nchildren ? optree_from_ast(a->children[0]) : NULL;
    OpNode *n = optree_new(a->type == NODE_LITERAL  ? OP_LITERAL
                           : a->type == NODE_BINARY ? OP_BINARY
                           : a->type == NODE_UNARY  ? OP_UNARY
                           : a->type == NODE_CALL   ? OP_CALL
                           : a->type == NODE_INDEX  ? OP_INDEX
                           : a->type == NODE_TYPE   ? OP_ARRAY_TYPE
                                                    : OP_IDENTIFIER);
    n->value = dupstr(a->label);
    if (a->type == NODE_BINARY || a->type == NODE_UNARY)
        n->op = dupstr(a->label);
    if (a->type == NODE_CALL)
        n->op = dupstr(a->children[0]->label);
    for (int i = 0; i < a->nchildren; i++) {
        OpNode *child = optree_from_ast(a->children[i]);
        if (child) {
            n->children = realloc(n->children, (n->nchildren + 1) * sizeof(*n->children));
            n->children[n->nchildren++] = child;
        }
    }
    return n;
}

void optree_free(OpNode *node) {
    if (!node)
        return;
    for (int i = 0; i < node->nchildren; i++) {
        optree_free(node->children[i]);
    }
    free(node->children);
    free(node->value);
    free(node->op);
    free(node->type_info);
    free(node);
}

static void optree_dot_node(FILE *f, OpNode *node, int *id) {
    if (!node)
        return;
    int myid = (*id)++;

    const char *label = "";
    const char *shape = "oval";

    switch (node->type) {
    case OP_LITERAL:
        label = node->value ? node->value : "";
        break;
    case OP_IDENTIFIER:
        label = node->value ? node->value : "";
        break;
    case OP_BINARY:
        label = node->op ? node->op : "+";
        break;
    case OP_UNARY:
        label = node->op ? node->op : "!";
        break;
    case OP_CALL:
        label = node->op ? node->op : "call";
        break;
    case OP_INDEX:
        label = "[]";
        break;
    case OP_ARRAY_TYPE:
        label = "type";
        break;
    }

    fprintf(f, "  node%d [label=\"%s\",shape=%s];\n", myid, label, shape);

    for (int i = 0; i < node->nchildren; i++) {
        int childid = *id;
        optree_dot_node(f, node->children[i], id);
        fprintf(f, "  node%d -> node%d;\n", myid, childid);
    }
}

void optree_to_dot(FILE *f, OpNode *root) {
    if (!root)
        return;
    fprintf(f, "digraph OpTree {\n");
    int id = 0;
    optree_dot_node(f, root, &id);
    fprintf(f, "}\n");
}
