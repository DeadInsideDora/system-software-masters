#ifndef OPTREE_H
#define OPTREE_H

#include "ast.h"

typedef enum OpNodeType {
    OP_LITERAL,
    OP_IDENTIFIER,
    OP_BINARY,
    OP_UNARY,
    OP_CALL,
    OP_INDEX,
    OP_ARRAY_TYPE
} OpNodeType;

typedef struct OpNode {
    OpNodeType type;
    char *value;
    char *op;
    char *type_info;
    struct OpNode **children;
    int nchildren;
} OpNode;

OpNode *optree_new(OpNodeType type);
OpNode *optree_literal(const char *value);
OpNode *optree_identifier(const char *name);
OpNode *optree_binary(const char *op, OpNode *left, OpNode *right);
OpNode *optree_unary(const char *op, OpNode *operand);
OpNode *optree_call(const char *func_name, OpNode **args, int nargs);
OpNode *optree_index(OpNode *array, OpNode *index);

OpNode *optree_from_ast(AST *ast_node);
void optree_free(OpNode *node);
void optree_to_dot(FILE *f, OpNode *root);

#endif
