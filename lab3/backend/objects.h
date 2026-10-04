#ifndef SPO_OBJECTS_H
#define SPO_OBJECTS_H
#include "ast.h"
#include <stddef.h>

typedef struct {
    const char *name;
    const AST *type;
    const AST *initializer;
} ObjectField;
typedef struct {
    const char *name;
    const AST *definition;
    AST *type;
    int owner;
} ObjectMethod;
typedef struct {
    const AST *definition;
    const char *name;
    int parent, state;
    ObjectField *fields;
    size_t field_count;
    ObjectMethod *methods;
    size_t method_count;
    const AST *constructor;
} ObjectClass;
typedef struct {
    ObjectClass *classes;
    size_t count;
    unsigned temporary;
    char *error;
} ObjectProgram;

const ObjectClass *objects_find(const ObjectProgram *, const char *);
const ObjectField *objects_field(const ObjectClass *, const char *);
const ObjectMethod *objects_method(const ObjectClass *, const char *);
int objects_subtype(const ObjectProgram *, const char *, const char *);
int objects_prepare(ObjectProgram *, AST **, int);
void objects_free(ObjectProgram *);
#endif
