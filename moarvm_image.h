#ifndef MOARVM_IMAGE_H
#define MOARVM_IMAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t bytecode_offset;
    uint32_t bytecode_size;
    uint32_t num_locals;
    uint32_t num_lexicals;
    uint32_t cuuid_string_index;
    uint32_t name_string_index;
    uint16_t outer_index;
    uint32_t annotation_offset;
    uint32_t num_annotations;
    uint32_t num_handlers;
    uint16_t flags;
    uint16_t num_static_lex_values;
    uint32_t code_obj_sc_dep_idx;
    uint32_t code_obj_sc_idx;
    uint32_t num_local_debug_names;
    uint16_t *local_types;
} MoarVMFrame;

typedef struct {
    uint8_t *arg_flags;
    uint16_t flag_count;
} MoarVMCallsite;

typedef struct {
    char **strings;
    size_t string_count;
    size_t string_capacity;

    MoarVMCallsite *callsites;
    size_t callsite_count;
    size_t callsite_capacity;

    MoarVMFrame *frames;
    size_t frame_count;
    size_t frame_capacity;

    uint8_t *bytecode;
    size_t bytecode_size;
    size_t bytecode_capacity;

    uint32_t hll_name_string_index;
    uint32_t main_frame_index;
    uint32_t mainline_frame_index;
    uint32_t load_frame_index;
    uint32_t deserialize_frame_index;
} MoarVMImage;

void moarvm_image_init(MoarVMImage *image);
void moarvm_image_free(MoarVMImage *image);
uint32_t moarvm_image_add_string(MoarVMImage *image, const char *text);
uint32_t moarvm_image_append_bytecode_u16(MoarVMImage *image, uint16_t value);
uint32_t moarvm_image_append_bytecode_u32(MoarVMImage *image, uint32_t value);
uint32_t moarvm_image_append_bytecode_u64(MoarVMImage *image, uint64_t value);
uint32_t moarvm_image_add_callsite(MoarVMImage *image, const uint8_t *arg_flags,
                                   uint16_t flag_count);
uint32_t moarvm_image_add_frame(MoarVMImage *image, const MoarVMFrame *frame);
int moarvm_frame_set_local_types(MoarVMFrame *frame, const uint16_t *types, uint32_t count);
void moarvm_frame_clear(MoarVMFrame *frame);
void moarvm_frame_init(MoarVMFrame *frame);
void moarvm_image_patch_u32(MoarVMImage *image, uint32_t offset, uint32_t value);
int moarvm_image_write_file(const MoarVMImage *image, const char *path);

int moarvm_write_minimal_program(const char *path);
int moarvm_write_arith_program(const char *path);

#endif
