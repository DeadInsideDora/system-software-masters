#ifndef MOARVM_MODEL_H
#define MOARVM_MODEL_H
#include <stddef.h>
#include "cfg.h"
#include "objects.h"

typedef enum {
    MV_DYNAMIC,
    MV_INT,
    MV_STRING,
    MV_INT_ARRAY,
    MV_STRING_ARRAY,
    MV_VOID,
    MV_FUNCTION,
    MV_OBJECT,
    MV_OBJECT_ARRAY,
    MV_INVALID
} MoarVMType;
typedef struct {
    char *name;
    MoarVMType type;
    int explicit_decl;
    int is_param;
    const AST *type_info;
    int is_function;
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
    int parent_index;
    const AST *return_type_info;
    AST *value_type;
} MoarVMFunctionModel;
typedef struct {
    MoarVMFunctionModel *functions;
    size_t function_count;
    char *error;
    const ObjectProgram *objects;
} MoarVMProgramModel;

MoarVMType moarvm_type_from_ast(const AST *type);
const char *moarvm_type_name(MoarVMType type);
const MoarVMFunctionModel *moarvm_find_function(const MoarVMProgramModel *, const char *);
const MoarVMVariableModel *moarvm_find_variable(const MoarVMFunctionModel *, const char *);
const MoarVMVariableModel *moarvm_resolve_variable(const MoarVMProgramModel *,
                                                   const MoarVMFunctionModel *, const char *,
                                                   unsigned *depth, size_t *slot);
int moarvm_types_equal(const AST *, const AST *);
void moarvm_write_type(FILE *, const AST *);
int moarvm_build_program_model(AnalysisResult **analyses, int count, const ObjectProgram *,
                               MoarVMProgramModel *out);
void moarvm_free_program_model(MoarVMProgramModel *model);
int moarvm_write_model(FILE *out, const MoarVMProgramModel *model);
#endif
