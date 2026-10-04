#include "moarvm_direct.h"
#include "moarvm_ops.h"
#include <inttypes.h>
#include <string.h>

static uint64_t read_le(const uint8_t *p, size_t bytes) {
    uint64_t n = 0;
    for (size_t i = 0; i < bytes; i++)
        n |= (uint64_t)p[i] << (8 * i);
    return n;
}
static void quoted(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n')
            fputs("\\n", out);
        else if (*p == '\r')
            fputs("\\r", out);
        else if (*p == '\t')
            fputs("\\t", out);
        else if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else
            fputc(*p, out);
    }
    fputc('"', out);
}
int moarvm_dump_image(FILE *out, const MoarVMImage *image) {
    fprintf(out, "MoarVM bytecode version 7; HLL=%s; entry frame=%u; load frame=",
            image->strings[image->hll_name_string_index], image->main_frame_index - 1);
    if (image->load_frame_index)
        fprintf(out, "%u\n", image->load_frame_index - 1);
    else
        fputs("none\n", out);
    fprintf(out, "%zu frames, %zu callsites, %zu strings, %zu bytecode bytes\n", image->frame_count,
            image->callsite_count, image->string_count, image->bytecode_size);
    for (size_t i = 0; i < image->string_count; i++) {
        fprintf(out, "string[%zu] = ", i);
        quoted(out, image->strings[i]);
        fputc('\n', out);
    }
    for (size_t i = 0; i < image->callsite_count; i++) {
        fprintf(out, "callsite[%zu]:", i);
        for (int j = 0; j < image->callsites[i].flag_count; j++) {
            unsigned flag = image->callsites[i].arg_flags[j];
            fprintf(out, " %s%s",
                    (flag & 15) == 2   ? "int"
                    : (flag & 15) == 8 ? "str"
                                       : "obj",
                    flag & 64 ? "(flat)" : "");
        }
        fputc('\n', out);
    }
    for (size_t i = 0; i < image->frame_count; i++) {
        const MoarVMFrame *frame = &image->frames[i];
        fprintf(out, "\nframe[%zu] %s offset=%u size=%u outer=%u\n", i,
                image->strings[frame->name_string_index], frame->bytecode_offset,
                frame->bytecode_size, frame->outer_index);
        for (uint32_t j = 0; j < frame->num_lexicals; j++)
            fprintf(out, "  lexical[%u] %s: %s\n", j,
                    image->strings[frame->lexical_name_indices[j]],
                    frame->lexical_types[j] == 4   ? "int64"
                    : frame->lexical_types[j] == 7 ? "str"
                                                   : "obj");
        for (uint32_t j = 0; j < frame->num_locals; j++)
            fprintf(out, "  r%u: %s\n", j,
                    frame->local_types[j] == 4   ? "int64"
                    : frame->local_types[j] == 7 ? "str"
                                                 : "obj");
        uint32_t offset = frame->bytecode_offset, end = offset + frame->bytecode_size;
        while (offset < end) {
            if (end - offset < 2)
                return -1;
            uint16_t code = (uint16_t)read_le(image->bytecode + offset, 2);
            const MoarVMOp *op = NULL;
            for (size_t k = 0; k < sizeof(moarvm_ops) / sizeof(*moarvm_ops); k++)
                if (moarvm_ops[k].code == code)
                    op = &moarvm_ops[k];
            if (!op)
                return -1;
            fprintf(out, "  %06u  %-18s", offset - frame->bytecode_offset, op->name);
            offset += 2;
            uint64_t last = 0;
            for (const char *p = op->operands; *p; p++) {
                size_t width = *p == 'q' ? 8 : (*p == 'r' || *p == 'h') ? 2 : 4;
                if (end - offset < width)
                    return -1;
                last = read_le(image->bytecode + offset, width);
                offset += (uint32_t)width;
                if (*p == 'r')
                    fprintf(out, " r%" PRIu64, last);
                else if (*p == 's')
                    fprintf(out, " str[%" PRIu64 "]", last);
                else if (*p == 'q')
                    fprintf(out, " %" PRId64, (int64_t)last);
                else
                    fprintf(out, " %" PRIu64, last);
            }
            if (!strncmp(op->name, "dispatch_", 9)) {
                if (last >= image->callsite_count)
                    return -1;
                uint16_t count = image->callsites[last].flag_count;
                for (uint16_t j = 0; j < count; j++) {
                    if (end - offset < 2)
                        return -1;
                    fprintf(out, " r%" PRIu64, read_le(image->bytecode + offset, 2));
                    offset += 2;
                }
            }
            fputc('\n', out);
        }
    }
    return ferror(out) ? -1 : 0;
}
