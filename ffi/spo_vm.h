#ifndef SPO_VM_H
#define SPO_VM_H
#include <stddef.h>
#include <stdint.h>
typedef struct SpoVM SpoVM;
typedef struct SpoValue SpoValue;
struct SpoValue {
    char type;
    int64_t integer;
    char *string;
    size_t length;
    SpoValue *items;
};
SpoVM *spo_vm_open(const char *path, char *error, size_t error_size);
void spo_vm_close(SpoVM *vm);
size_t spo_vm_export_count(const SpoVM *vm);
const char *spo_vm_export_name(const SpoVM *vm, size_t index);
const char *spo_vm_signature(const SpoVM *vm, const char *name);
int spo_vm_call(SpoVM *vm, const char *name, const SpoValue *args, size_t count, SpoValue *result);
const char *spo_vm_error(const SpoVM *vm);
void spo_value_clear(SpoValue *value);
#endif
