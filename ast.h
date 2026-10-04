#ifndef AST_H
#define AST_H

#include <stdio.h>

typedef enum NodeType {
    NODE_PROGRAM,
    NODE_FUNCTION,
    NODE_PARAM_LIST,
    NODE_PARAM,
    NODE_BLOCK,
    NODE_VAR_DECL,
    NODE_IF,
    NODE_WHILE,
    NODE_DO_WHILE,
    NODE_BREAK,
    NODE_EXPR_STMT,
    NODE_RETURN,
    NODE_CALL,
    NODE_INDEX,
    NODE_BINARY,
    NODE_UNARY,
    NODE_LITERAL,
    NODE_IDENTIFIER,
    NODE_TYPE,
    NODE_CONTINUE
} NodeType;

typedef struct AST {
    NodeType type;
    char *label;
    struct AST **children;
    int nchildren;
} AST;

AST *ast_new(NodeType type, const char *label);
void ast_append(AST *parent, AST *child);
AST *ast_copy(AST *source);
void ast_free(AST *a);
void ast_to_dot(FILE *f, AST *root);

#endif
