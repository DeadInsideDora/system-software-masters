#include "moarvm_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOARVM_HEADER_SIZE 96u
#define MOARVM_BYTECODE_VERSION 7u
#define MOARVM_FRAME_HEADER_SIZE 54u

#define MOARVM_SCDEP_HEADER_OFFSET 12u
#define MOARVM_EXTOP_HEADER_OFFSET 20u
#define MOARVM_FRAME_HEADER_OFFSET 28u
#define MOARVM_CALLSITE_HEADER_OFFSET 36u
#define MOARVM_STRING_HEADER_OFFSET 44u
#define MOARVM_SCDATA_HEADER_OFFSET 52u
#define MOARVM_BYTECODE_HEADER_OFFSET 60u
#define MOARVM_ANNOTATION_HEADER_OFFSET 68u
#define MOARVM_HLL_NAME_HEADER_OFFSET 76u
#define MOARVM_SPECIAL_FRAME_HEADER_OFFSET 80u

#define MVM_OP_RETURN 55u
#define MVM_OP_EQ_I 56u
#define MVM_OP_ADD_I 63u
#define MVM_OP_GOTO 23u
#define MVM_OP_IF_I 24u
#define MVM_OP_RETURN_I 51u
#define MVM_OP_CONST_I64_16 597u

#define MVM_REG_INT64 4u

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ByteBuffer;

static char *dupstr_local(const char *s) {
    size_t len;
    char *copy;
    if (!s)
        return NULL;
    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static int buffer_reserve(ByteBuffer *buf, size_t extra) {
    size_t needed = buf->size + extra;
    size_t next_cap;
    uint8_t *new_data;
    if (needed <= buf->capacity)
        return 0;
    next_cap = buf->capacity ? buf->capacity : 128;
    while (next_cap < needed)
        next_cap *= 2;
    new_data = (uint8_t *)realloc(buf->data, next_cap);
    if (!new_data)
        return -1;
    buf->data = new_data;
    buf->capacity = next_cap;
    return 0;
}

static int buffer_append_u8(ByteBuffer *buf, uint8_t value) {
    if (buffer_reserve(buf, 1) != 0)
        return -1;
    buf->data[buf->size++] = value;
    return 0;
}

static int buffer_append_u16(ByteBuffer *buf, uint16_t value) {
    if (buffer_reserve(buf, 2) != 0)
        return -1;
    buf->data[buf->size++] = (uint8_t)(value & 0xFFu);
    buf->data[buf->size++] = (uint8_t)((value >> 8) & 0xFFu);
    return 0;
}

static int buffer_append_u32(ByteBuffer *buf, uint32_t value) {
    if (buffer_reserve(buf, 4) != 0)
        return -1;
    buf->data[buf->size++] = (uint8_t)(value & 0xFFu);
    buf->data[buf->size++] = (uint8_t)((value >> 8) & 0xFFu);
    buf->data[buf->size++] = (uint8_t)((value >> 16) & 0xFFu);
    buf->data[buf->size++] = (uint8_t)((value >> 24) & 0xFFu);
    return 0;
}

static int buffer_append_bytes(ByteBuffer *buf, const void *src, size_t len) {
    if (buffer_reserve(buf, len) != 0)
        return -1;
    memcpy(buf->data + buf->size, src, len);
    buf->size += len;
    return 0;
}

static int buffer_append_pad4(ByteBuffer *buf) {
    while (buf->size % 4u != 0u) {
        if (buffer_append_u8(buf, 0) != 0)
            return -1;
    }
    return 0;
}

static void write_u32_at(uint8_t *dst, size_t offset, uint32_t value) {
    dst[offset + 0] = (uint8_t)(value & 0xFFu);
    dst[offset + 1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[offset + 2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[offset + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

void moarvm_image_init(MoarVMImage *image) {
    memset(image, 0, sizeof(*image));
}

void moarvm_image_free(MoarVMImage *image) {
    size_t i;
    if (!image)
        return;
    for (i = 0; i < image->string_count; ++i) {
        free(image->strings[i]);
    }
    for (i = 0; i < image->callsite_count; ++i) {
        free(image->callsites[i].arg_flags);
    }
    for (i = 0; i < image->frame_count; ++i) {
        moarvm_frame_clear(&image->frames[i]);
    }
    free(image->strings);
    free(image->callsites);
    free(image->frames);
    free(image->bytecode);
    memset(image, 0, sizeof(*image));
}

uint32_t moarvm_image_add_string(MoarVMImage *image, const char *text) {
    char **new_strings;
    size_t next_capacity;
    if (image->string_count == image->string_capacity) {
        next_capacity = image->string_capacity ? image->string_capacity * 2 : 8;
        new_strings = (char **)realloc(image->strings, next_capacity * sizeof(char *));
        if (!new_strings)
            return UINT32_MAX;
        image->strings = new_strings;
        image->string_capacity = next_capacity;
    }
    image->strings[image->string_count] = dupstr_local(text ? text : "");
    if (!image->strings[image->string_count])
        return UINT32_MAX;
    return (uint32_t)image->string_count++;
}

uint32_t moarvm_image_append_bytecode_u16(MoarVMImage *image, uint16_t value) {
    size_t needed = image->bytecode_size + 2;
    size_t next_capacity;
    uint8_t *new_bytecode;
    uint32_t offset = (uint32_t)image->bytecode_size;
    if (needed > image->bytecode_capacity) {
        next_capacity = image->bytecode_capacity ? image->bytecode_capacity * 2 : 64;
        while (next_capacity < needed)
            next_capacity *= 2;
        new_bytecode = (uint8_t *)realloc(image->bytecode, next_capacity);
        if (!new_bytecode)
            return UINT32_MAX;
        image->bytecode = new_bytecode;
        image->bytecode_capacity = next_capacity;
    }
    image->bytecode[image->bytecode_size++] = (uint8_t)(value & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 8) & 0xFFu);
    return offset;
}

uint32_t moarvm_image_append_bytecode_u32(MoarVMImage *image, uint32_t value) {
    size_t needed = image->bytecode_size + 4;
    size_t next_capacity;
    uint8_t *new_bytecode;
    uint32_t offset = (uint32_t)image->bytecode_size;
    if (needed > image->bytecode_capacity) {
        next_capacity = image->bytecode_capacity ? image->bytecode_capacity * 2 : 64;
        while (next_capacity < needed)
            next_capacity *= 2;
        new_bytecode = (uint8_t *)realloc(image->bytecode, next_capacity);
        if (!new_bytecode)
            return UINT32_MAX;
        image->bytecode = new_bytecode;
        image->bytecode_capacity = next_capacity;
    }
    image->bytecode[image->bytecode_size++] = (uint8_t)(value & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 8) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 16) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 24) & 0xFFu);
    return offset;
}

uint32_t moarvm_image_append_bytecode_u64(MoarVMImage *image, uint64_t value) {
    size_t needed = image->bytecode_size + 8;
    size_t next_capacity;
    uint8_t *new_bytecode;
    uint32_t offset = (uint32_t)image->bytecode_size;
    if (needed > image->bytecode_capacity) {
        next_capacity = image->bytecode_capacity ? image->bytecode_capacity * 2 : 64;
        while (next_capacity < needed)
            next_capacity *= 2;
        new_bytecode = (uint8_t *)realloc(image->bytecode, next_capacity);
        if (!new_bytecode)
            return UINT32_MAX;
        image->bytecode = new_bytecode;
        image->bytecode_capacity = next_capacity;
    }
    image->bytecode[image->bytecode_size++] = (uint8_t)(value & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 8) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 16) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 24) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 32) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 40) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 48) & 0xFFu);
    image->bytecode[image->bytecode_size++] = (uint8_t)((value >> 56) & 0xFFu);
    return offset;
}

uint32_t moarvm_image_add_callsite(MoarVMImage *image, const uint8_t *arg_flags,
                                   uint16_t flag_count) {
    MoarVMCallsite *new_callsites;
    MoarVMCallsite *dst;
    size_t next_capacity;

    if (!image)
        return UINT32_MAX;
    if (image->callsite_count == image->callsite_capacity) {
        next_capacity = image->callsite_capacity ? image->callsite_capacity * 2 : 4;
        new_callsites =
            (MoarVMCallsite *)realloc(image->callsites, next_capacity * sizeof(MoarVMCallsite));
        if (!new_callsites)
            return UINT32_MAX;
        image->callsites = new_callsites;
        image->callsite_capacity = next_capacity;
    }

    dst = &image->callsites[image->callsite_count];
    memset(dst, 0, sizeof(*dst));
    dst->flag_count = flag_count;
    if (flag_count > 0) {
        dst->arg_flags = (uint8_t *)malloc((size_t)flag_count);
        if (!dst->arg_flags)
            return UINT32_MAX;
        memcpy(dst->arg_flags, arg_flags, (size_t)flag_count);
    }
    return (uint32_t)image->callsite_count++;
}

void moarvm_frame_init(MoarVMFrame *frame) {
    memset(frame, 0, sizeof(*frame));
}

void moarvm_frame_clear(MoarVMFrame *frame) {
    if (!frame)
        return;
    free(frame->local_types);
    free(frame->lexical_types);
    free(frame->lexical_name_indices);
    memset(frame, 0, sizeof(*frame));
}

int moarvm_frame_set_local_types(MoarVMFrame *frame, const uint16_t *types, uint32_t count) {
    size_t bytes;
    if (!frame)
        return -1;
    free(frame->local_types);
    frame->local_types = NULL;
    frame->num_locals = count;
    if (count == 0)
        return 0;
    bytes = (size_t)count * sizeof(uint16_t);
    frame->local_types = (uint16_t *)malloc(bytes);
    if (!frame->local_types)
        return -1;
    memcpy(frame->local_types, types, bytes);
    return 0;
}

uint32_t moarvm_image_add_frame(MoarVMImage *image, const MoarVMFrame *frame) {
    MoarVMFrame *new_frames;
    size_t next_capacity;
    MoarVMFrame *dst;
    if (image->frame_count == image->frame_capacity) {
        next_capacity = image->frame_capacity ? image->frame_capacity * 2 : 4;
        new_frames = (MoarVMFrame *)realloc(image->frames, next_capacity * sizeof(MoarVMFrame));
        if (!new_frames)
            return UINT32_MAX;
        image->frames = new_frames;
        image->frame_capacity = next_capacity;
    }
    dst = &image->frames[image->frame_count];
    *dst = *frame;
    dst->local_types = NULL;
    dst->lexical_types = NULL;
    dst->lexical_name_indices = NULL;
    if (frame->num_locals) {
        size_t bytes = (size_t)frame->num_locals * sizeof(uint16_t);
        dst->local_types = (uint16_t *)malloc(bytes);
        if (!dst->local_types)
            return UINT32_MAX;
        memcpy(dst->local_types, frame->local_types, bytes);
    }
    if (frame->num_lexicals) {
        size_t types_size = frame->num_lexicals * sizeof(*dst->lexical_types);
        size_t names_size = frame->num_lexicals * sizeof(*dst->lexical_name_indices);
        dst->lexical_types = malloc(types_size);
        dst->lexical_name_indices = malloc(names_size);
        if (!dst->lexical_types || !dst->lexical_name_indices) {
            moarvm_frame_clear(dst);
            return UINT32_MAX;
        }
        memcpy(dst->lexical_types, frame->lexical_types, types_size);
        memcpy(dst->lexical_name_indices, frame->lexical_name_indices, names_size);
    }
    return (uint32_t)image->frame_count++;
}

void moarvm_image_patch_u32(MoarVMImage *image, uint32_t offset, uint32_t value) {
    if (!image || offset + 4 > image->bytecode_size)
        return;
    image->bytecode[offset + 0] = (uint8_t)(value & 0xFFu);
    image->bytecode[offset + 1] = (uint8_t)((value >> 8) & 0xFFu);
    image->bytecode[offset + 2] = (uint8_t)((value >> 16) & 0xFFu);
    image->bytecode[offset + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

static int serialize_strings(const MoarVMImage *image, ByteBuffer *buf) {
    size_t i;
    for (i = 0; i < image->string_count; ++i) {
        const char *text = image->strings[i] ? image->strings[i] : "";
        size_t len = strlen(text);
        uint32_t header = ((uint32_t)len << 1) | 1u;
        if (buffer_append_u32(buf, header) != 0)
            return -1;
        if (buffer_append_bytes(buf, text, len) != 0)
            return -1;
        if (buffer_append_pad4(buf) != 0)
            return -1;
    }
    return 0;
}

static int serialize_frames(const MoarVMImage *image, ByteBuffer *buf) {
    size_t i;
    for (i = 0; i < image->frame_count; ++i) {
        const MoarVMFrame *frame = &image->frames[i];
        if (buffer_append_u32(buf, frame->bytecode_offset) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->bytecode_size) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->num_locals) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->num_lexicals) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->cuuid_string_index) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->name_string_index) != 0)
            return -1;
        if (buffer_append_u16(buf, frame->outer_index) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->annotation_offset) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->num_annotations) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->num_handlers) != 0)
            return -1;
        if (buffer_append_u16(buf, frame->flags) != 0)
            return -1;
        if (buffer_append_u16(buf, frame->num_static_lex_values) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->code_obj_sc_dep_idx) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->code_obj_sc_idx) != 0)
            return -1;
        if (buffer_append_u32(buf, frame->num_local_debug_names) != 0)
            return -1;
        if (frame->num_locals) {
            size_t j;
            for (j = 0; j < frame->num_locals; ++j) {
                if (buffer_append_u16(buf, frame->local_types[j]) != 0)
                    return -1;
            }
        }
        for (uint32_t j = 0; j < frame->num_lexicals; j++) {
            if (buffer_append_u16(buf, frame->lexical_types[j]) != 0 ||
                buffer_append_u32(buf, frame->lexical_name_indices[j]) != 0)
                return -1;
        }
    }
    return 0;
}

static int serialize_callsites(const MoarVMImage *image, ByteBuffer *buf) {
    size_t i;
    for (i = 0; i < image->callsite_count; ++i) {
        const MoarVMCallsite *callsite = &image->callsites[i];
        if (buffer_append_u16(buf, callsite->flag_count) != 0)
            return -1;
        if (callsite->flag_count > 0) {
            if (buffer_append_bytes(buf, callsite->arg_flags, callsite->flag_count) != 0)
                return -1;
            if (callsite->flag_count % 2u != 0u) {
                if (buffer_append_u8(buf, 0) != 0)
                    return -1;
            }
        }
    }
    return 0;
}

int moarvm_image_write_file(const MoarVMImage *image, const char *path) {
    ByteBuffer strings = {0};
    ByteBuffer frames = {0};
    ByteBuffer callsites = {0};
    ByteBuffer final = {0};
    uint32_t cur_offset = MOARVM_HEADER_SIZE;
    uint32_t scdep_offset = cur_offset;
    uint32_t extop_offset = cur_offset;
    uint32_t frame_offset;
    uint32_t callsite_offset;
    uint32_t string_offset;
    uint32_t scdata_offset;
    uint32_t bytecode_offset;
    uint32_t annotation_offset;
    FILE *out = NULL;
    int rc = -1;

    if (!image || !path)
        return -1;
    if (serialize_frames(image, &frames) != 0)
        goto cleanup;
    if (serialize_callsites(image, &callsites) != 0)
        goto cleanup;
    if (serialize_strings(image, &strings) != 0)
        goto cleanup;

    frame_offset = cur_offset;
    cur_offset += (uint32_t)frames.size;
    callsite_offset = cur_offset;
    cur_offset += (uint32_t)callsites.size;
    string_offset = cur_offset;
    cur_offset += (uint32_t)strings.size;
    scdata_offset = cur_offset;
    bytecode_offset = cur_offset;
    cur_offset += (uint32_t)image->bytecode_size;
    annotation_offset = cur_offset;

    if (buffer_reserve(&final, cur_offset) != 0)
        goto cleanup;
    memset(final.data, 0, cur_offset);
    final.size = cur_offset;

    memcpy(final.data, "MOARVM\r\n", 8);
    write_u32_at(final.data, 8, MOARVM_BYTECODE_VERSION);
    write_u32_at(final.data, MOARVM_SCDEP_HEADER_OFFSET, scdep_offset);
    write_u32_at(final.data, MOARVM_SCDEP_HEADER_OFFSET + 4, 0);
    write_u32_at(final.data, MOARVM_EXTOP_HEADER_OFFSET, extop_offset);
    write_u32_at(final.data, MOARVM_EXTOP_HEADER_OFFSET + 4, 0);
    write_u32_at(final.data, MOARVM_FRAME_HEADER_OFFSET, frame_offset);
    write_u32_at(final.data, MOARVM_FRAME_HEADER_OFFSET + 4, (uint32_t)image->frame_count);
    write_u32_at(final.data, MOARVM_CALLSITE_HEADER_OFFSET, callsite_offset);
    write_u32_at(final.data, MOARVM_CALLSITE_HEADER_OFFSET + 4, (uint32_t)image->callsite_count);
    write_u32_at(final.data, MOARVM_STRING_HEADER_OFFSET, string_offset);
    write_u32_at(final.data, MOARVM_STRING_HEADER_OFFSET + 4, (uint32_t)image->string_count);
    write_u32_at(final.data, MOARVM_SCDATA_HEADER_OFFSET, scdata_offset);
    write_u32_at(final.data, MOARVM_SCDATA_HEADER_OFFSET + 4, 0);
    write_u32_at(final.data, MOARVM_BYTECODE_HEADER_OFFSET, bytecode_offset);
    write_u32_at(final.data, MOARVM_BYTECODE_HEADER_OFFSET + 4, (uint32_t)image->bytecode_size);
    write_u32_at(final.data, MOARVM_ANNOTATION_HEADER_OFFSET, annotation_offset);
    write_u32_at(final.data, MOARVM_ANNOTATION_HEADER_OFFSET + 4, 0);
    write_u32_at(final.data, MOARVM_HLL_NAME_HEADER_OFFSET, image->hll_name_string_index);
    write_u32_at(final.data, MOARVM_SPECIAL_FRAME_HEADER_OFFSET, image->mainline_frame_index);
    write_u32_at(final.data, MOARVM_SPECIAL_FRAME_HEADER_OFFSET + 4, image->main_frame_index);
    write_u32_at(final.data, MOARVM_SPECIAL_FRAME_HEADER_OFFSET + 8, image->load_frame_index);
    write_u32_at(final.data, MOARVM_SPECIAL_FRAME_HEADER_OFFSET + 12,
                 image->deserialize_frame_index);

    memcpy(final.data + frame_offset, frames.data, frames.size);
    memcpy(final.data + callsite_offset, callsites.data, callsites.size);
    memcpy(final.data + string_offset, strings.data, strings.size);
    memcpy(final.data + bytecode_offset, image->bytecode, image->bytecode_size);

    out = fopen(path, "wb");
    if (!out)
        goto cleanup;
    if (fwrite(final.data, 1, final.size, out) != final.size)
        goto cleanup;
    if (fclose(out) != 0) {
        out = NULL;
        goto cleanup;
    }
    out = NULL;
    rc = 0;

cleanup:
    if (out)
        fclose(out);
    free(strings.data);
    free(frames.data);
    free(callsites.data);
    free(final.data);
    return rc;
}

int moarvm_write_minimal_program(const char *path) {
    MoarVMImage image;
    MoarVMFrame frame;
    uint32_t hll_idx;
    uint32_t name_idx;
    uint32_t cuuid_idx;
    uint32_t bc_offset;
    int rc;

    moarvm_image_init(&image);

    hll_idx = moarvm_image_add_string(&image, "nqp");
    name_idx = moarvm_image_add_string(&image, "main");
    cuuid_idx = moarvm_image_add_string(&image, "spo-minimal-cu");
    if (hll_idx == UINT32_MAX || name_idx == UINT32_MAX || cuuid_idx == UINT32_MAX) {
        moarvm_image_free(&image);
        return -1;
    }

    bc_offset = moarvm_image_append_bytecode_u16(&image, (uint16_t)MVM_OP_RETURN);
    if (bc_offset == UINT32_MAX) {
        moarvm_image_free(&image);
        return -1;
    }

    moarvm_frame_init(&frame);
    frame.bytecode_offset = bc_offset;
    frame.bytecode_size = 2;
    frame.cuuid_string_index = cuuid_idx;
    frame.name_string_index = name_idx;
    frame.outer_index = 0;
    if (moarvm_image_add_frame(&image, &frame) == UINT32_MAX) {
        moarvm_image_free(&image);
        return -1;
    }

    image.hll_name_string_index = hll_idx;
    image.main_frame_index = 1;
    image.mainline_frame_index = 0;
    image.load_frame_index = 0;
    image.deserialize_frame_index = 0;

    rc = moarvm_image_write_file(&image, path);
    moarvm_frame_clear(&frame);
    moarvm_image_free(&image);
    return rc;
}

static int emit_const_i64_16(MoarVMImage *image, uint16_t dst, int16_t literal) {
    if (moarvm_image_append_bytecode_u16(image, (uint16_t)MVM_OP_CONST_I64_16) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, dst) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, (uint16_t)literal) == UINT32_MAX)
        return -1;
    return 0;
}

static int emit_add_i(MoarVMImage *image, uint16_t dst, uint16_t lhs, uint16_t rhs) {
    if (moarvm_image_append_bytecode_u16(image, (uint16_t)MVM_OP_ADD_I) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, dst) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, lhs) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, rhs) == UINT32_MAX)
        return -1;
    return 0;
}

static int emit_eq_i(MoarVMImage *image, uint16_t dst, uint16_t lhs, uint16_t rhs) {
    if (moarvm_image_append_bytecode_u16(image, (uint16_t)MVM_OP_EQ_I) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, dst) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, lhs) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, rhs) == UINT32_MAX)
        return -1;
    return 0;
}

static uint32_t emit_if_i_placeholder(MoarVMImage *image, uint16_t cond_reg) {
    uint32_t patch_at;
    if (moarvm_image_append_bytecode_u16(image, (uint16_t)MVM_OP_IF_I) == UINT32_MAX)
        return UINT32_MAX;
    if (moarvm_image_append_bytecode_u16(image, cond_reg) == UINT32_MAX)
        return UINT32_MAX;
    patch_at = (uint32_t)image->bytecode_size;
    if (moarvm_image_append_bytecode_u32(image, 0) == UINT32_MAX)
        return UINT32_MAX;
    return patch_at;
}

static uint32_t emit_goto_placeholder(MoarVMImage *image) {
    uint32_t patch_at;
    if (moarvm_image_append_bytecode_u16(image, (uint16_t)MVM_OP_GOTO) == UINT32_MAX)
        return UINT32_MAX;
    patch_at = (uint32_t)image->bytecode_size;
    if (moarvm_image_append_bytecode_u32(image, 0) == UINT32_MAX)
        return UINT32_MAX;
    return patch_at;
}

static int emit_return_i(MoarVMImage *image, uint16_t src) {
    if (moarvm_image_append_bytecode_u16(image, (uint16_t)MVM_OP_RETURN_I) == UINT32_MAX)
        return -1;
    if (moarvm_image_append_bytecode_u16(image, src) == UINT32_MAX)
        return -1;
    return 0;
}

int moarvm_write_arith_program(const char *path) {
    MoarVMImage image;
    MoarVMFrame frame;
    uint16_t local_types[6] = {MVM_REG_INT64, MVM_REG_INT64, MVM_REG_INT64,
                               MVM_REG_INT64, MVM_REG_INT64, MVM_REG_INT64};
    uint32_t hll_idx;
    uint32_t name_idx;
    uint32_t cuuid_idx;
    uint32_t if_patch;
    uint32_t goto_patch;
    uint32_t label_true;
    uint32_t label_end;
    int rc;

    moarvm_image_init(&image);
    moarvm_frame_init(&frame);

    hll_idx = moarvm_image_add_string(&image, "nqp");
    name_idx = moarvm_image_add_string(&image, "main");
    cuuid_idx = moarvm_image_add_string(&image, "spo-arith-cu");
    if (hll_idx == UINT32_MAX || name_idx == UINT32_MAX || cuuid_idx == UINT32_MAX)
        goto fail;
    if (moarvm_frame_set_local_types(&frame, local_types, 6) != 0)
        goto fail;

    if (emit_const_i64_16(&image, 0, 2) != 0)
        goto fail;
    if (emit_const_i64_16(&image, 1, 3) != 0)
        goto fail;
    if (emit_add_i(&image, 2, 0, 1) != 0)
        goto fail;
    if (emit_const_i64_16(&image, 3, 5) != 0)
        goto fail;
    if (emit_eq_i(&image, 4, 2, 3) != 0)
        goto fail;
    if_patch = emit_if_i_placeholder(&image, 4);
    if (if_patch == UINT32_MAX)
        goto fail;

    if (emit_const_i64_16(&image, 5, 7) != 0)
        goto fail;
    goto_patch = emit_goto_placeholder(&image);
    if (goto_patch == UINT32_MAX)
        goto fail;

    label_true = (uint32_t)image.bytecode_size;
    moarvm_image_patch_u32(&image, if_patch, label_true);
    if (emit_const_i64_16(&image, 5, 42) != 0)
        goto fail;

    label_end = (uint32_t)image.bytecode_size;
    moarvm_image_patch_u32(&image, goto_patch, label_end);
    if (emit_return_i(&image, 5) != 0)
        goto fail;

    frame.bytecode_offset = 0;
    frame.bytecode_size = (uint32_t)image.bytecode_size;
    frame.cuuid_string_index = cuuid_idx;
    frame.name_string_index = name_idx;
    frame.outer_index = 0;
    if (moarvm_image_add_frame(&image, &frame) == UINT32_MAX)
        goto fail;

    image.hll_name_string_index = hll_idx;
    image.main_frame_index = 1;
    image.mainline_frame_index = 0;
    image.load_frame_index = 0;
    image.deserialize_frame_index = 0;

    rc = moarvm_image_write_file(&image, path);
    moarvm_frame_clear(&frame);
    moarvm_image_free(&image);
    return rc;

fail:
    moarvm_frame_clear(&frame);
    moarvm_image_free(&image);
    return -1;
}
