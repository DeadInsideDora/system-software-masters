%{
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "parse_module.h"
static AST *node(NodeType t, const char *s, AST *a, AST *b) {
    AST *n = ast_new(t, s); ast_append(n, a); ast_append(n, b); return n;
}
static AST *leaf(NodeType t, char *s) {
    AST *n = ast_new(t, s); free(s); return n;
}
static AST *array_type(AST *dimensions, AST *element) {
    AST *inner=dimensions;
    while(inner->nchildren)inner=inner->children[0];
    ast_append(inner,element);return dimensions;
}
%}
%pure-parser
%locations
%parse-param {ParseContext *ctx}
%parse-param {void *scanner}
%lex-param {void *scanner}
%union { char *str; AST *node; }
%{
int yylex(YYSTYPE *, YYLTYPE *, void *);
void yyerror(YYLTYPE *, ParseContext *, void *, const char *);
%}
%token <str> ID TYPE LITERAL RELOP ASSIGN
%token IF ELSE WHILE DO BREAK CONTINUE RETURN AND OR INVALID
%type <node> items item signature params param_list param type vars var statement statements block
%type <node> expr assignment logical_or logical_and bit_or bit_and comparison sum product unary postfix primary args arg_list
%type <node> base_type array_dims
%destructor { free($$); } ID TYPE LITERAL RELOP ASSIGN
%destructor { ast_free($$); } items item signature params param_list param type vars var statement statements block expr assignment logical_or logical_and bit_or bit_and comparison sum product unary postfix primary args arg_list
%destructor { ast_free($$); } base_type array_dims
%nonassoc IF_WITHOUT_ELSE
%nonassoc ELSE
%start source
%%
source: items { ctx->root = $1; };
items: { $$ = node(NODE_PROGRAM,"program",NULL,NULL); }
     | items item { ast_append($1,$2); $$=$1; };
item: signature block { $$=node(NODE_FUNCTION,$1->label,$1,$2); }
    | signature ';' { $$=node(NODE_FUNCTION,$1->label,$1,NULL); };
signature: ID '(' params ')' { $$=leaf(NODE_PARAM_LIST,$1); ast_append($$,$3); }
    | type ID '(' params ')' { $$=leaf(NODE_PARAM_LIST,$2); ast_append($$,$1); ast_append($$,$4); };
params: { $$=node(NODE_PARAM_LIST,"params",NULL,NULL); } | param_list { $$=$1; };
param_list: param { $$=node(NODE_PARAM_LIST,"params",$1,NULL); }
    | param_list ',' param { ast_append($1,$3); $$=$1; };
param: ID { $$=leaf(NODE_PARAM,$1); }
    | type ID { $$=leaf(NODE_PARAM,$2); ast_append($$,$1); };
base_type: TYPE { $$=leaf(NODE_TYPE,$1); }
    | TYPE array_dims { $$=array_type($2,leaf(NODE_TYPE,$1)); };
array_dims: '[' ']' { $$=node(NODE_TYPE,"array",NULL,NULL); }
    | array_dims '[' ']' { $$=node(NODE_TYPE,"array",$1,NULL); };
type: base_type { $$=$1; };
vars: var { $$=node(NODE_PARAM_LIST,"vars",$1,NULL); }
    | vars ',' var { ast_append($1,$3); $$=$1; };
var: ID { $$=leaf(NODE_VAR_DECL,$1); }
    | ID '=' expr { $$=leaf(NODE_VAR_DECL,$1); ast_append($$,$3); };
statement: type vars ';' {
        for(int i=0;i<$2->nchildren;i++) ast_append($2->children[i],ast_copy($1));
        ast_free($1); $$=$2;
    }
    | IF '(' expr ')' statement %prec IF_WITHOUT_ELSE { $$=node(NODE_IF,"if",$3,$5); }
    | IF '(' expr ')' statement ELSE statement { $$=node(NODE_IF,"if",$3,$5); ast_append($$,$7); }
    | WHILE '(' expr ')' statement { $$=node(NODE_WHILE,"while",$3,$5); }
    | DO block WHILE '(' expr ')' ';' { $$=node(NODE_DO_WHILE,"do",$2,$5); }
    | BREAK ';' { $$=node(NODE_BREAK,"break",NULL,NULL); }
    | CONTINUE ';' { $$=node(NODE_CONTINUE,"continue",NULL,NULL); }
    | RETURN expr ';' { $$=node(NODE_RETURN,"return",$2,NULL); }
    | RETURN ';' { $$=node(NODE_RETURN,"return",NULL,NULL); }
    | block { $$=$1; }
    | expr ';' { $$=node(NODE_EXPR_STMT,"expression",$1,NULL); }
    | ';' { $$=node(NODE_BLOCK,"empty",NULL,NULL); };
statements: { $$=node(NODE_BLOCK,"block",NULL,NULL); }
    | statements statement { ast_append($1,$2); $$=$1; };
block: '{' statements '}' { $$=$2; };
expr: assignment { $$=$1; };
assignment: logical_or { $$=$1; }
    | logical_or '=' assignment { $$=node(NODE_BINARY,"=",$1,$3); }
    | logical_or ASSIGN assignment { $$=leaf(NODE_BINARY,$2); ast_append($$,$1); ast_append($$,$3); };
logical_or: logical_and { $$=$1; }
    | logical_or OR logical_and { $$=node(NODE_BINARY,"||",$1,$3); };
logical_and: bit_or { $$=$1; }
    | logical_and AND bit_or { $$=node(NODE_BINARY,"&&",$1,$3); };
bit_or: bit_and { $$=$1; } | bit_or '|' bit_and { $$=node(NODE_BINARY,"|",$1,$3); };
bit_and: comparison { $$=$1; } | bit_and '&' comparison { $$=node(NODE_BINARY,"&",$1,$3); };
comparison: sum { $$=$1; }
    | comparison RELOP sum { $$=leaf(NODE_BINARY,$2); ast_append($$,$1); ast_append($$,$3); };
sum: product { $$=$1; }
    | sum '+' product { $$=node(NODE_BINARY,"+",$1,$3); }
    | sum '-' product { $$=node(NODE_BINARY,"-",$1,$3); };
product: unary { $$=$1; }
    | product '*' unary { $$=node(NODE_BINARY,"*",$1,$3); }
    | product '/' unary { $$=node(NODE_BINARY,"/",$1,$3); }
    | product '%' unary { $$=node(NODE_BINARY,"%",$1,$3); };
unary: postfix { $$=$1; }
    | '-' unary { $$=node(NODE_UNARY,"-",$2,NULL); }
    | '!' unary { $$=node(NODE_UNARY,"!",$2,NULL); }
    | '+' unary { $$=node(NODE_UNARY,"+",$2,NULL); };
postfix: primary { $$=$1; }
    | postfix '(' args ')' { $$=node(NODE_CALL,"call",$1,$3); }
    | postfix '[' expr ']' { $$=node(NODE_INDEX,"index",$1,node(NODE_PARAM_LIST,"args",$3,NULL)); };
primary: ID { $$=leaf(NODE_IDENTIFIER,$1); }
    | LITERAL { $$=leaf(NODE_LITERAL,$1); }
    | '(' expr ')' { $$=$2; };
args: { $$=node(NODE_PARAM_LIST,"args",NULL,NULL); } | arg_list { $$=$1; };
arg_list: expr { $$=node(NODE_PARAM_LIST,"args",$1,NULL); }
    | arg_list ',' expr { ast_append($1,$3); $$=$1; };
%%
void yyerror(YYLTYPE *loc, ParseContext *ctx, void *scanner, const char *s) {
    (void)scanner; report_parse_error(ctx,loc->first_line,s);
}
