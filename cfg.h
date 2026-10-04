#ifndef CFG_H
#define CFG_H

#include "ast.h"
#include "optree.h"

typedef struct CFGNode CFGNode;
typedef enum { CFG_NOP, CFG_EXPR, CFG_DECL, CFG_CONDITION, CFG_RETURN } CFGNodeKind;

struct CFGNode {
    int id;
    CFGNodeKind kind;
    AST *operation;
    OpNode *optree;
    char *label_override;
    CFGNode *nextDefault;
    CFGNode *nextConditional;
    char *condLabel;
};

typedef struct FunctionCFG {
    char *name;
    AST *signature;
    char *source_file;
    CFGNode **nodes;
    int nnodes;
    CFGNode *entry;
    CFGNode *exit;
    int has_body;
    struct FunctionCFG *outer;
} FunctionCFG;

typedef struct AnalysisResult {
    FunctionCFG **functions;
    int nfunctions;
    char **errors;
    int nerrors;
} AnalysisResult;

AnalysisResult *build_cfg_from_ast(AST *root, const char *source_file);
void free_analysis(AnalysisResult *res);

void write_function_dot(FILE *f, FunctionCFG *fn);
void write_callgraph_dot(FILE *f, AnalysisResult *res);

#endif
