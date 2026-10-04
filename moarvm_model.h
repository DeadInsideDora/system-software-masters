#ifndef MOARVM_MODEL_H
#define MOARVM_MODEL_H
#include <stddef.h>
#include "cfg.h"

typedef enum {
    MV_DYNAMIC,
    MV_INT,
    MV_STRING,
    MV_INT_ARRAY,
    MV_STRING_ARRAY,
    MV_VOID,
    MV_INVALID
} MoarVMType;
typedef struct {
    char *name;
    MoarVMType type;
    int explicit_decl;
    int is_param;
    const AST *type_info;
} MoarVMVariableModel;
typedef struct {
    char *source_name;
    const FunctionCFG *cfg;
    AST *signature;
    MoarVMType return_type;
    MoarVMVariableModel *vars;
    size_t var_count;
    size_t param_count;
    int has_body;
    const AST *return_type_info;
} MoarVMFunctionModel;
typedef struct {
    MoarVMFunctionModel *functions;
    size_t function_count;
    char *error;
} MoarVMProgramModel;

MoarVMType moarvm_type_from_ast(const AST *type);
const char *moarvm_type_name(MoarVMType type);
const MoarVMFunctionModel *moarvm_find_function(const MoarVMProgramModel *, const char *);
const MoarVMVariableModel *moarvm_find_variable(const MoarVMFunctionModel *, const char *);
int moarvm_types_equal(const AST *, const AST *);
void moarvm_write_type(FILE *, const AST *);
int moarvm_build_program_model(AnalysisResult **analyses, int count,
                               MoarVMProgramModel *out);
void moarvm_free_program_model(MoarVMProgramModel *model);
int moarvm_write_model(FILE *out, const MoarVMProgramModel *model);
#endif
