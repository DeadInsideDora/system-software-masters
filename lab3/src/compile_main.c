#include "parse_module.h"
#include "moarvm_direct.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
#define GC_CONSTANT(symbol, number) symbol = number,
#include "gc_layout.def"
#undef GC_CONSTANT
};

static int number(const char *text, int64_t *value) {
    char *end;
    errno = 0;
    if (!*text || *text < '0' || *text > '9') return -1;
    int64_t n = strtoll(text, &end, 10);
    if (errno || *end || n < 0) return -1;
    *value = n;
    return 0;
}

static AST *parse(const char *text) {
    char **errors = NULL;
    int count = 0;
    AST *tree = parse_string(text, &errors, &count);
    for (int i = 0; i < count; i++) { fprintf(stderr, "%s\n", errors[i]); free(errors[i]); }
    free(errors);
    return tree;
}

static AST *read_source(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "cannot open source: %s\n", path); return NULL; }
    AST *tree = NULL;
    char *text = NULL;
    long length;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        length >= INT32_MAX || fseek(file, 0, SEEK_SET)) goto done;
    text = malloc((size_t)length + 1);
    if (!text) goto done;
    if (fread(text, 1, (size_t)length, file) != (size_t)length ||
        memchr(text, 0, (size_t)length)) goto done;
    text[length] = 0;
    tree = parse(text);
done:
    free(text);
    if (fclose(file)) { ast_free(tree); tree = NULL; }
    if (!tree) fprintf(stderr, "cannot parse source: %s\n", path);
    return tree;
}

static int constant(const char *name, int64_t *value) {
#define GC_CONSTANT(symbol, number) if (!strcmp(name, #symbol)) { *value = number; return 1; }
#include "gc_layout.def"
#undef GC_CONSTANT
    return 0;
}

static void label(AST *node, const char *text) {
    free(node->label);
    node->label = strdup(text);
}

static int lower(AST *node, const int64_t *args, size_t count) {
    int64_t value;
    if ((node->type == NODE_VAR_DECL || node->type == NODE_PARAM || node->type == NODE_FUNCTION) &&
        (constant(node->label, &value) || !strncmp(node->label, "__lab3_", 7) ||
         !strcmp(node->label, "vm_load") || !strcmp(node->label, "vm_store") ||
         !strcmp(node->label, "vm_words") || !strcmp(node->label, "vm_arg") ||
         !strcmp(node->label, "vm_stop") || !strcmp(node->label, "vm_out") ||
         !strcmp(node->label, "vm_trace") || !strcmp(node->label, "vm_trace_gc") || !strcmp(node->label, "vm_time_ns"))) {
        fprintf(stderr, "reserved name: %s\n", node->label); return -1;
    }
    if (node->type == NODE_BINARY && node->nchildren &&
        (!strcmp(node->label, "=") || !strcmp(node->label, "+=") || !strcmp(node->label, "-=") ||
         !strcmp(node->label, "*=") || !strcmp(node->label, "/=") || !strcmp(node->label, "%=")) &&
        constant(node->children[0]->label, &value)) {
        fprintf(stderr, "reserved name: %s\n", node->children[0]->label); return -1;
    }
    if (node->type == NODE_CALL && !strcmp(node->children[0]->label, "vm_arg")) {
        AST *params = node->children[1];
        if (params->nchildren != 1 || params->children[0]->type != NODE_LITERAL ||
            number(params->children[0]->label, &value) || (uint64_t)value >= count) {
            fprintf(stderr, "vm_arg needs a configured constant argument index\n"); return -1;
        }
        value = args[value];
        for (int i = 0; i < node->nchildren; i++) ast_free(node->children[i]);
        free(node->children); node->children = NULL; node->nchildren = 0;
        node->type = NODE_LITERAL;
        char digits[32]; snprintf(digits, sizeof(digits), "%" PRId64, value); label(node, digits);
    } else if (node->type == NODE_IDENTIFIER && constant(node->label, &value)) {
        node->type = NODE_LITERAL;
        char digits[32]; snprintf(digits, sizeof(digits), "%" PRId64, value); label(node, digits);
    } else if ((node->type == NODE_FUNCTION || node->type == NODE_PARAM_LIST ||
                node->type == NODE_IDENTIFIER) && !strcmp(node->label, "main")) {
        label(node, "__lab3_entry");
    }
    for (int i = 0; i < node->nchildren; i++) if (lower(node->children[i], args, count)) return -1;
    return 0;
}

int main(int argc, char **argv) {
    int rc = 1, index = 1, mains = 0;
    int64_t heap_words = 0, profile_capacity = 0, *args = NULL;
    size_t count = 0;
    AST *tree = NULL;
    AnalysisResult *analysis = NULL;
    ObjectProgram objects = {0};
    MoarVMProgramModel model = {0};
    MoarVMImage image = {0};
    char *error = NULL, *temporary = NULL;
    while (index < argc && !strncmp(argv[index], "--", 2)) {
        const char *option = argv[index++];
        int64_t value;
        if (index == argc || number(argv[index++], &value)) goto usage;
        if (!strcmp(option, "--heap-words") && value > 0 && value <= INT32_MAX)
            heap_words = value;
        else if (!strcmp(option, "--profile-capacity") && value > 0 && value <= INT32_MAX / 9)
            profile_capacity = value;
        else if (!strcmp(option, "--arg")) {
            int64_t *next = realloc(args, (count + 1) * sizeof(*args));
            if (!next) goto done;
            args = next; args[count++] = value;
        } else goto usage;
    }
    if (!heap_words || argc - index < 2) goto usage;
    const char *output = argv[index++];
    char wrapper[2048];
    snprintf(wrapper, sizeof(wrapper),
        "int main() { int[] __lab3_memory = new_int_array(%" PRId64 ");"
        "int vm_words() { return %" PRId64 "; }"
        "int vm_load(int address) {"
        "if (address < 0 || address >= vm_words()) vm_stop(%d);"
        "return __lab3_memory[address]; }"
        "int vm_store(int address, int value) {"
        "if (address < 0 || address >= vm_words()) vm_stop(%d);"
        "__lab3_memory[address] = value; return 0; } }", heap_words, heap_words, GC_BAD_ADDRESS, GC_BAD_ADDRESS);
    tree = parse(wrapper);
    if (!tree) goto done;
    AST *body = tree->children[0]->children[1];
    char profile[4096];
    if (profile_capacity) {
        snprintf(profile, sizeof(profile),
            "int main() { int[] __lab3_trace_data = new_int_array(%" PRId64 ");"
            "int __lab3_trace_count = 0; int __lab3_trace_start = 0;"
            "void vm_trace(int heap, int event) {"
            "int now = vm_time_ns();"
            "if (__lab3_trace_count == 0) __lab3_trace_start = now;"
            "if (__lab3_trace_count >= %" PRId64 ") vm_stop(%d);"
            "int pos = __lab3_trace_count * 9;"
            "__lab3_trace_data[pos] = now - __lab3_trace_start;"
            "__lab3_trace_data[pos+1] = event;"
            "__lab3_trace_data[pos+2] = vm_load(heap+%d) - vm_load(heap+%d);"
            "__lab3_trace_data[pos+3] = vm_load(heap+%d);"
            "__lab3_trace_data[pos+4] = vm_load(heap+%d);"
            "__lab3_trace_data[pos+5] = 0; __lab3_trace_data[pos+6] = 0;"
            "__lab3_trace_data[pos+7] = 0; __lab3_trace_data[pos+8] = 0;"
            "__lab3_trace_count += 1; }"
            "void vm_trace_gc(int heap, int survivors, int root_updates, int edge_updates) {"
            "vm_trace(heap, %d); int pos = (__lab3_trace_count - 1) * 9;"
            "__lab3_trace_data[pos+5] = survivors;"
            "__lab3_trace_data[pos+6] = vm_load(heap+%d);"
            "__lab3_trace_data[pos+7] = root_updates;"
            "__lab3_trace_data[pos+8] = edge_updates; } }",
            profile_capacity * 9, profile_capacity, GC_TRACE_OVERFLOW,
            GC_FREE, GC_FROM, GC_COLLECTIONS, GC_ALLOCATED_OBJECTS,
            GC_TRACE_AFTER, GC_LAST_COPIED_OBJECTS);
    } else {
        snprintf(profile, sizeof(profile), "int main(){ void vm_trace(int heap,int event){} "
                 "void vm_trace_gc(int heap,int survivors,int root_updates,int edge_updates){} }");
    }
    AST *profile_tree = parse(profile);
    if (!profile_tree) goto done;
    AST *profile_body = profile_tree->children[0]->children[1];
    for (int i = 0; i < profile_body->nchildren; i++) ast_append(body, profile_body->children[i]);
    profile_body->nchildren = 0; ast_free(profile_tree);

    for (; index < argc; index++) {
        struct stat input_stat, output_stat;
        if (!strcmp(output, argv[index]) ||
            (!stat(output, &output_stat) && !stat(argv[index], &input_stat) &&
             output_stat.st_dev == input_stat.st_dev && output_stat.st_ino == input_stat.st_ino)) {
            fprintf(stderr, "output must differ from source\n"); goto done;
        }
        AST *input = read_source(argv[index]);
        if (!input) goto done;
        for (int i = 0; i < input->nchildren; i++) {
            AST *fn = input->children[i];
            if (fn->type != NODE_FUNCTION || fn->nchildren != 2) {
                fprintf(stderr, "expected function definitions\n"); ast_free(input); goto done;
            }
            if (!strcmp(fn->label, "main")) {
                AST *sig = fn->children[0];
                if (sig->children[sig->nchildren - 1]->nchildren) {
                    fprintf(stderr, "expected main with no parameters\n"); ast_free(input); goto done;
                }
                mains++;
            }
        }
        if (lower(input, args, count)) { ast_free(input); goto done; }
        for (int i = 0; i < input->nchildren; i++) ast_append(body, input->children[i]);
        input->nchildren = 0; ast_free(input);
    }
    if (mains != 1) { fprintf(stderr, "expected main defined exactly once\n"); goto done; }
    const char *tail_source = profile_capacity ?
        "int main() { __lab3_entry(); int row=0; while(row<__lab3_trace_count) {"
        "print(\"TRACE\"); int col=0; while(col<9) { vm_out(44);"
        "print_int(__lab3_trace_data[row*9+col]); col+=1; } vm_out(10); row+=1; } return 0; }" :
        "int main() { __lab3_entry(); return 0; }";
    AST *tail = parse(tail_source);
    if (!tail) goto done;
    AST *tail_body = tail->children[0]->children[1];
    for (int i = 0; i < tail_body->nchildren; i++) ast_append(body, tail_body->children[i]);
    tail_body->nchildren = 0; ast_free(tail);
    if (objects_prepare(&objects, &tree, 1)) { fprintf(stderr, "%s\n", objects.error); goto done; }
    analysis = build_cfg_from_ast(tree, "<lab3>");
    for (int i = 0; i < analysis->nerrors; i++) fprintf(stderr, "%s\n", analysis->errors[i]);
    if (analysis->nerrors) goto done;
    if (moarvm_build_program_model(&analysis, 1, &objects, &model)) {
        fprintf(stderr, "%s\n", model.error); goto done;
    }
    if (moarvm_generate_image(&model, &image, &error)) {
        fprintf(stderr, "MoarVM generation failed: %s\n", error); goto done;
    }
    size_t size = strlen(output) + sizeof(".tmp.XXXXXX");
    temporary = malloc(size);
    if (!temporary) goto done;
    snprintf(temporary, size, "%s.tmp.XXXXXX", output);
    int fd = mkstemp(temporary);
    if (fd < 0) { perror(output); goto done; }
    if (close(fd) || moarvm_image_write_file(&image, temporary) || rename(temporary, output)) {
        fprintf(stderr, "cannot write bytecode: %s\n", output); goto done;
    }
    rc = 0;
    goto done;
usage:
    fprintf(stderr, "usage: lab3c --heap-words N [--arg N ...] [--profile-capacity N] output.moarvm source.src ...\n"
                    "heap words must be in 1..2147483647; arguments in 0..9223372036854775807\n");
done:
    if (temporary) { if (rc) remove(temporary); free(temporary); }
    free(error); free(args);
    moarvm_image_free(&image); moarvm_free_program_model(&model); objects_free(&objects);
    free_analysis(analysis); ast_free(tree);
    return rc;
}
