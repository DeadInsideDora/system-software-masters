#include "moar.h"
#include "spo_vm.h"
#include "moarvm_ffi_abi.h"
#include "ffi_runtime.h"
#include <unistd.h>

struct SpoVM {
    MVMInstance *instance;
    MVMCompUnit *runtime;
    MVMObject *request, *exports;
    MVMString *hll, *request_key;
    char **names, **signatures;
    size_t count;
    pid_t owner;
    char error[1024];
};
static int fail(SpoVM *vm, const char *message) {
    snprintf(vm->error, sizeof(vm->error), "%s", message);
    return -1;
}
void spo_value_clear(SpoValue *value) {
    if (!value)
        return;
    for (size_t i = 0; value->items && i < value->length; i++)
        spo_value_clear(&value->items[i]);
    free(value->items);
    free(value->string);
    memset(value, 0, sizeof(*value));
}
static int object_kind(MVMThreadContext *tc, MVMObject *obj, MVMuint32 repr) {
    return !MVM_is_null(tc, obj) && IS_CONCRETE(obj) && REPR(obj)->ID == repr;
}
static int array_kind(MVMThreadContext *tc, MVMObject *obj, MVMuint16 slots) {
    return object_kind(tc, obj, MVM_REPR_ID_VMArray) &&
           ((MVMArrayREPRData *)STABLE(obj)->REPR_data)->slot_type == slots;
}
static char *string_copy(MVMThreadContext *tc, MVMString *str, size_t *length) {
    if (!str) {
        *length = 0;
        return strdup("");
    }
    char *copy = NULL;
    MVMROOT(tc, str) {
        MVMuint64 size = 0;
        char *bytes = MVM_string_utf8_encode(tc, str, &size, 0);
        copy = malloc((size_t)size + 1);
        if (copy) {
            memcpy(copy, bytes, (size_t)size);
            copy[size] = 0;
            *length = (size_t)size;
        }
        MVM_free(bytes);
    }
    return copy;
}
static MVMObject *box_string(MVMThreadContext *tc, const char *bytes, size_t length) {
    MVMString *str = MVM_string_utf8_decode(tc, tc->instance->VMString, bytes, length);
    return MVM_repr_box_str(tc, tc->instance->boot_types.BOOTStr, str);
}
static MVMObject *to_vm(MVMThreadContext *tc, const SpoValue *value) {
    if (value->type == 'i')
        return MVM_repr_box_int(tc, tc->instance->boot_types.BOOTInt, value->integer);
    if (value->type == 's')
        return box_string(tc, value->string, value->length);
    MVMObject *array =
        MVM_repr_alloc_init(tc, value->type == 'I' ? tc->instance->boot_types.BOOTIntArray
                                                   : tc->instance->boot_types.BOOTStrArray);
    MVMROOT(tc, array) {
        for (size_t i = 0; i < value->length; i++) {
            if (value->type == 'I')
                MVM_repr_push_i(tc, array, value->items[i].integer);
            else {
                MVMString *str = MVM_string_utf8_decode(
                    tc, tc->instance->VMString, value->items[i].string, value->items[i].length);
                MVMROOT(tc, str) {
                    MVM_repr_push_s(tc, array, str);
                }
            }
        }
    }
    return array;
}
static int from_vm(SpoVM *vm, MVMObject *object, char type, SpoValue *out) {
    MVMThreadContext *tc = vm->instance->main_thread;
    int status = 0;
    out->type = type;
    MVMROOT(tc, object) {
        if (type == 'v') {
        } else if (type == 'i' && object_kind(tc, object, MVM_REPR_ID_P6int))
            out->integer = MVM_repr_get_int(tc, object);
        else if (type == 's' && object_kind(tc, object, MVM_REPR_ID_P6str)) {
            out->string = string_copy(tc, MVM_repr_get_str(tc, object), &out->length);
            if (!out->string)
                status = fail(vm, "out of memory copying result string");
        } else if ((type == 'I' || type == 'S') &&
                   array_kind(tc, object, type == 'I' ? MVM_ARRAY_I64 : MVM_ARRAY_STR)) {
            out->length = (size_t)MVM_repr_elems(tc, object);
            out->items = calloc(out->length ? out->length : 1, sizeof(*out->items));
            if (!out->items)
                status = fail(vm, "out of memory copying result array");
            for (size_t i = 0; i < out->length && !status; i++) {
                SpoValue *item = &out->items[i];
                item->type = type == 'I' ? 'i' : 's';
                if (type == 'I')
                    item->integer = MVM_repr_at_pos_i(tc, object, i);
                else {
                    item->string = string_copy(tc, MVM_repr_at_pos_s(tc, object, i), &item->length);
                    if (!item->string)
                        status = fail(vm, "out of memory copying array element");
                }
            }
        } else
            status = fail(vm, "VM result does not match the exported signature");
    }
    if (status)
        spo_value_clear(out);
    return status;
}
static void initial_invoke(MVMThreadContext *tc, void *frame) {
    MVM_frame_dispatch_zero_args(tc, ((MVMStaticFrame *)frame)->body.static_code);
}
static void clear_request(SpoVM *vm) {
    MVMThreadContext *tc = vm->instance->main_thread;
    for (int i = 0; i < SPO_FFI_SLOTS; i++)
        MVM_repr_bind_pos_o(tc, vm->request, i, tc->instance->VMNull);
}
static int run(SpoVM *vm, int frame) {
    MVMThreadContext *tc = vm->instance->main_thread;
    MVM_interp_run(tc, initial_invoke, ((MVMCode *)vm->runtime->body.coderefs[frame])->body.sf,
                   NULL);
    MVMObject *error = MVM_repr_at_pos_o(tc, vm->request, SPO_FFI_ERROR);
    if (!MVM_is_null(tc, error)) {
        size_t length = 0;
        char *text = string_copy(tc, MVM_repr_get_str(tc, error), &length);
        fail(vm, text ? text : "MoarVM error");
        free(text);
        return -1;
    }
    return 0;
}
static int read_exports(SpoVM *vm) {
    MVMThreadContext *tc = vm->instance->main_thread;
    MVMString *key = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, SPO_FFI_VERSION);
    MVMObject *version = MVM_hll_sym_get(tc, vm->hll, key);
    if (!object_kind(tc, version, MVM_REPR_ID_P6int) ||
        MVM_repr_get_int(tc, version) != SPO_FFI_ABI_VERSION)
        return fail(vm, "not a SPO library: compile with --library (ABI version 1 required)");
    key = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, SPO_FFI_EXPORTS);
    vm->exports = MVM_hll_sym_get(tc, vm->hll, key);
    if (!array_kind(tc, vm->exports, MVM_ARRAY_OBJ))
        return fail(vm, "invalid export table");
    vm->count = (size_t)MVM_repr_elems(tc, vm->exports);
    vm->names = calloc(vm->count ? vm->count : 1, sizeof(*vm->names));
    vm->signatures = calloc(vm->count ? vm->count : 1, sizeof(*vm->signatures));
    if (!vm->names || !vm->signatures)
        return fail(vm, "out of memory reading exports");
    for (size_t i = 0; i < vm->count; i++) {
        MVMObject *entry = MVM_repr_at_pos_o(tc, vm->exports, i);
        if (!array_kind(tc, entry, MVM_ARRAY_OBJ) || MVM_repr_elems(tc, entry) != 3)
            return fail(vm, "invalid export entry");
        size_t length = 0;
        MVMObject *name = MVM_repr_at_pos_o(tc, entry, SPO_EXPORT_NAME);
        if (!object_kind(tc, name, MVM_REPR_ID_P6str))
            return fail(vm, "invalid exported name");
        vm->names[i] = string_copy(tc, MVM_repr_get_str(tc, name), &length);
        entry = MVM_repr_at_pos_o(tc, vm->exports, i);
        MVMObject *signature = MVM_repr_at_pos_o(tc, entry, SPO_EXPORT_SIGNATURE);
        if (!object_kind(tc, signature, MVM_REPR_ID_P6str))
            return fail(vm, "invalid exported signature");
        vm->signatures[i] = string_copy(tc, MVM_repr_get_str(tc, signature), &length);
        if (!vm->names[i] || !vm->signatures[i])
            return fail(vm, "out of memory reading export");
        const char *s = vm->signatures[i];
        if (length < 3 || !strchr("isISv", s[0]) || s[1] != '(' || s[length - 1] != ')')
            return fail(vm, "invalid exported signature format");
        for (size_t j = 2; j + 1 < length; j++)
            if (!strchr("isIS", s[j]))
                return fail(vm, "unsupported exported argument type");
        entry = MVM_repr_at_pos_o(tc, vm->exports, i);
        if (!object_kind(tc, MVM_repr_at_pos_o(tc, entry, SPO_EXPORT_CODE), MVM_REPR_ID_MVMCode))
            return fail(vm, "invalid exported code object");
    }
    return 0;
}
SpoVM *spo_vm_open(const char *path, char *error, size_t error_size) {
    SpoVM *vm = calloc(1, sizeof(*vm));
    if (!vm) {
        snprintf(error, error_size, "out of memory creating VM");
        return NULL;
    }
    vm->owner = getpid();
    vm->instance = MVM_vm_create_instance();
    if (!vm->instance->cross_thread_write_logging)
        uv_mutex_init(&vm->instance->mutex_cross_thread_write_logging);
    MVMThreadContext *tc = vm->instance->main_thread;
    MVM_spesh_worker_stop(tc);
    MVM_spesh_worker_join(tc);
    vm->instance->spesh_enabled = 0;
    vm->instance->jit_enabled = 0;
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&vm->runtime);
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&vm->request);
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&vm->exports);
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&vm->hll);
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&vm->request_key);
    MVMuint8 *bytes = MVM_malloc(sizeof(spo_ffi_runtime));
    memcpy(bytes, spo_ffi_runtime, sizeof(spo_ffi_runtime));
    vm->runtime = MVM_cu_from_bytes(tc, bytes, sizeof(spo_ffi_runtime));
    vm->runtime->body.deallocate = MVM_DEALLOCATE_FREE;
    vm->hll = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, SPO_FFI_HLL);
    vm->request_key = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, SPO_FFI_REQUEST);
    vm->request = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTArray);
    MVM_hll_sym_get(tc, vm->hll, vm->request_key);
    MVMObject *table = MVM_repr_at_key_o(tc, tc->instance->hll_syms, vm->hll);
    MVM_repr_bind_key_o(tc, table, vm->request_key, vm->request);
    clear_request(vm);
    MVMObject *filename = box_string(tc, path, strlen(path));
    MVM_repr_bind_pos_o(tc, vm->request, SPO_FFI_CODE, filename);
    if (run(vm, SPO_RUNTIME_LOAD) || read_exports(vm)) {
        snprintf(error, error_size, "%s", vm->error);
        spo_vm_close(vm);
        return NULL;
    }
    clear_request(vm);
    return vm;
}
void spo_vm_close(SpoVM *vm) {
    if (!vm)
        return;
    if (vm->instance && vm->owner == getpid())
        MVM_vm_destroy_instance(vm->instance);
    for (size_t i = 0; i < vm->count; i++) {
        if (vm->names)
            free(vm->names[i]);
        if (vm->signatures)
            free(vm->signatures[i]);
    }
    free(vm->names);
    free(vm->signatures);
    free(vm);
}
size_t spo_vm_export_count(const SpoVM *vm) {
    return vm->count;
}
const char *spo_vm_export_name(const SpoVM *vm, size_t index) {
    return index < vm->count ? vm->names[index] : NULL;
}
const char *spo_vm_signature(const SpoVM *vm, const char *name) {
    for (size_t i = 0; i < vm->count; i++)
        if (!strcmp(vm->names[i], name))
            return vm->signatures[i];
    return NULL;
}
const char *spo_vm_error(const SpoVM *vm) {
    return vm->error;
}
int spo_vm_call(SpoVM *vm, const char *name, const SpoValue *args, size_t count, SpoValue *result) {
    memset(result, 0, sizeof(*result));
    vm->error[0] = 0;
    if (vm->owner != getpid())
        return fail(vm, "VM handles cannot be used after fork");
    size_t index = 0;
    while (index < vm->count && strcmp(vm->names[index], name))
        index++;
    if (index == vm->count)
        return fail(vm, "unknown exported function");
    const char *signature = vm->signatures[index];
    if (count != strlen(signature) - 3)
        return fail(vm, "wrong argument count");
    for (size_t i = 0; i < count; i++)
        if (args[i].type != signature[i + 2])
            return fail(vm, "argument type mismatch");
    MVMThreadContext *tc = vm->instance->main_thread;
    clear_request(vm);
    MVMObject *entry = MVM_repr_at_pos_o(tc, vm->exports, index);
    MVM_repr_bind_pos_o(tc, vm->request, SPO_FFI_CODE,
                        MVM_repr_at_pos_o(tc, entry, SPO_EXPORT_CODE));
    MVMObject *array = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTArray);
    MVM_repr_bind_pos_o(tc, vm->request, SPO_FFI_ARGS, array);
    for (size_t i = 0; i < count; i++) {
        MVMObject *value = to_vm(tc, &args[i]);
        array = MVM_repr_at_pos_o(tc, vm->request, SPO_FFI_ARGS);
        MVM_repr_push_o(tc, array, value);
    }
    MVMObject *is_void =
        MVM_repr_box_int(tc, tc->instance->boot_types.BOOTInt, signature[0] == 'v');
    MVM_repr_bind_pos_o(tc, vm->request, SPO_FFI_VOID, is_void);
    int status = run(vm, SPO_RUNTIME_CALL);
    if (!status)
        status =
            from_vm(vm, MVM_repr_at_pos_o(tc, vm->request, SPO_FFI_RESULT), signature[0], result);
    clear_request(vm);
    return status;
}
