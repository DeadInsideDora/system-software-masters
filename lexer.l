%option reentrant bison-bridge bison-locations noyywrap yylineno noinput nounput
%option extra-type="ParseContext *"
%x COMMENT
%{
#include "parse_module.h"
#include "parser.tab.h"
#include <stdlib.h>
#include <string.h>
#define YY_USER_ACTION yylloc->first_line = yylloc->last_line = yylineno;
%}
%%
[ \t\r\n]+ ;
"//"[^\n]* ;
"/*" BEGIN(COMMENT);
<COMMENT>"*/" BEGIN(INITIAL);
<COMMENT>[^*\n]+ ;
<COMMENT>"*" ;
<COMMENT>\n ;
<COMMENT><<EOF>> { report_parse_error(yyextra,yylineno,"unterminated comment"); BEGIN(INITIAL); return 0; }
"if" return IF;
"else" return ELSE;
"while" return WHILE;
"do" return DO;
"break" return BREAK;
"continue" return CONTINUE;
"return" return RETURN;
"fn" return FN;
"class" return CLASS;
"match" return MATCH;
"this" return THIS;
"super" return SUPER;
"=>" return FATARROW;
"->" return ARROW;
"bool"|"byte"|"int"|"uint"|"long"|"ulong"|"char"|"string"|"void" { yylval->str=strdup(yytext); return TYPE; }
"true"|"false"|"null" { yylval->str=strdup(yytext); return LITERAL; }
0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+ { yylval->str=strdup(yytext); return LITERAL; }
\"([^\"\\\n]|\\.)*\" { yylval->str=strdup(yytext); return LITERAL; }
\'([^\'\\\n]|\\.)\' { yylval->str=strdup(yytext); return LITERAL; }
[a-zA-Z_][a-zA-Z0-9_]* { yylval->str=strdup(yytext); return parse_is_type(yyextra,yytext)?USER_TYPE:ID; }
"=="|"!="|"<="|">="|"<"|">" { yylval->str=strdup(yytext); return RELOP; }
"+="|"-="|"*="|"/="|"%=" { yylval->str=strdup(yytext); return ASSIGN; }
"&&" return AND;
"||" return OR;
[=+*/%!&|(){}\[\],;:.\-] return (unsigned char)yytext[0];
. { report_parse_error(yyextra,yylineno,"invalid character or unterminated literal"); return INVALID; }
%%
