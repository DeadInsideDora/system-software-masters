#ifndef PARSE_MODULE_H
#define PARSE_MODULE_H

#include "ast.h"

typedef struct ParseContext {
    AST *root;
    char **errors;
    int nerrors;
} ParseContext;
void report_parse_error(ParseContext *ctx, int line, const char *message);
AST *parse_string(const char *input, char ***out_errors, int *out_nerrors);

#endif
