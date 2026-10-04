#ifndef PARSE_MODULE_H
#define PARSE_MODULE_H

#include "ast.h"

typedef struct ParseContext {
    AST *root;
    char **errors;
    int nerrors;
    char **type_names;
    int ntypes;
} ParseContext;
void report_parse_error(ParseContext *ctx, int line, const char *message);
AST *parse_string(const char *input, char ***out_errors, int *out_nerrors);
AST *parse_string_types(const char *, char **, int, char ***, int *);
void parse_collect_types(const char *, char ***, int *);
int parse_is_type(ParseContext *, const char *);

#endif
