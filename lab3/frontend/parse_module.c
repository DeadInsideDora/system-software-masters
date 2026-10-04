#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse_module.h"
#include "parser.tab.h"
typedef void *yyscan_t;
typedef struct yy_buffer_state *YY_BUFFER_STATE;
int yylex_init_extra(ParseContext *, yyscan_t *);
int yylex_destroy(yyscan_t);
YY_BUFFER_STATE yy_scan_string(const char *, yyscan_t);
void yy_delete_buffer(YY_BUFFER_STATE, yyscan_t);
void yyset_lineno(int, yyscan_t);
int yyparse(ParseContext *, yyscan_t);
int yylex(YYSTYPE *, YYLTYPE *, yyscan_t);
int parse_is_type(ParseContext *ctx, const char *name) {
    for (int i = 0; i < ctx->ntypes; i++)
        if (!strcmp(ctx->type_names[i], name))
            return 1;
    return 0;
}
void parse_collect_types(const char *input, char ***names, int *count) {
    ParseContext ctx = {0};
    yyscan_t scanner = NULL;
    if (yylex_init_extra(&ctx, &scanner))
        return;
    YY_BUFFER_STATE buffer = yy_scan_string(input, scanner);
    yyset_lineno(1, scanner);
    YYSTYPE value;
    YYLTYPE loc = {0};
    int token, previous = 0;
    while ((token = yylex(&value, &loc, scanner))) {
        if (previous == CLASS && token == ID) {
            int found = 0;
            for (int i = 0; i < *count; i++)
                if (!strcmp((*names)[i], value.str))
                    found = 1;
            if (!found) {
                *names = realloc(*names, (*count + 1) * sizeof(**names));
                (*names)[(*count)++] = strdup(value.str);
            }
        }
        if (token == ID || token == TYPE || token == USER_TYPE || token == LITERAL ||
            token == RELOP || token == ASSIGN)
            free(value.str);
        previous = token;
    }
    yy_delete_buffer(buffer, scanner);
    yylex_destroy(scanner);
    for (int i = 0; i < ctx.nerrors; i++)
        free(ctx.errors[i]);
    free(ctx.errors);
}
void report_parse_error(ParseContext *ctx, int line, const char *message) {
    char text[1024];
    char **items = realloc(ctx->errors, (ctx->nerrors + 1) * sizeof(*items));
    if (!items)
        return;
    ctx->errors = items;
    snprintf(text, sizeof(text), "line %d: %s", line, message);
    ctx->errors[ctx->nerrors++] = strdup(text);
}
AST *parse_string_types(const char *input, char **names, int count, char ***out_errors,
                        int *out_nerrors) {
    ParseContext ctx = {.type_names = names, .ntypes = count};
    yyscan_t scanner = NULL;
    YY_BUFFER_STATE buffer = NULL;
    int status = 1;
    if (yylex_init_extra(&ctx, &scanner) == 0) {
        buffer = yy_scan_string(input, scanner);
        if (buffer) {
            yyset_lineno(1, scanner);
            status = yyparse(&ctx, scanner);
        }
        if (buffer)
            yy_delete_buffer(buffer, scanner);
        yylex_destroy(scanner);
    }
    if (status && !ctx.nerrors)
        report_parse_error(&ctx, 1, "parser failed");
    if (status || ctx.nerrors) {
        ast_free(ctx.root);
        ctx.root = NULL;
    }
    if (out_nerrors)
        *out_nerrors = ctx.nerrors;
    if (out_errors)
        *out_errors = ctx.errors;
    else {
        for (int i = 0; i < ctx.nerrors; ++i)
            free(ctx.errors[i]);
        free(ctx.errors);
    }
    return ctx.root;
}
AST *parse_string(const char *input, char ***out_errors, int *out_nerrors) {
    char **names = NULL;
    int count = 0;
    parse_collect_types(input, &names, &count);
    AST *tree = parse_string_types(input, names, count, out_errors, out_nerrors);
    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return tree;
}
