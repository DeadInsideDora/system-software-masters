#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "parse_module.h"
#include "moarvm_direct.h"

typedef struct {
    AST **trees;
    AST **original_trees;
    AnalysisResult **analyses;
    char **names;
    int count;
} Inputs;
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t size = 0, capacity = 4096;
    char *text = malloc(capacity);
    if (!text) {
        fclose(f);
        return NULL;
    }
    size_t n;
    do {
        if (capacity - size < 4097) {
            capacity *= 2;
            char *next = realloc(text, capacity);
            if (!next) {
                free(text);
                fclose(f);
                return NULL;
            }
            text = next;
        }
        n = fread(text + size, 1, 4096, f);
        size += n;
    } while (n == 4096);
    int bad = ferror(f);
    if (fclose(f))
        bad = 1;
    if (bad || memchr(text, 0, size)) {
        free(text);
        return NULL;
    }
    text[size] = 0;
    return text;
}
static int add_input(Inputs *in, const char *source, const char *name, int internal) {
    char **errors = NULL;
    int count = 0;
    AST *tree = parse_string(source, &errors, &count);
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "%s: %s\n", name, errors[i]);
        free(errors[i]);
    }
    free(errors);
    if (!tree)
        return -1;
    if (!internal)
        for (int i = 0; i < tree->nchildren; i++)
            if (!strncmp(tree->children[i]->label, "__spo_", 6)) {
                fprintf(stderr, "%s: function prefix __spo_ is reserved for the runtime\n", name);
                ast_free(tree);
                return -1;
            }
    in->trees = realloc(in->trees, (in->count + 1) * sizeof(*in->trees));
    in->analyses = realloc(in->analyses, (in->count + 1) * sizeof(*in->analyses));
    in->names = realloc(in->names, (in->count + 1) * sizeof(*in->names));
    in->trees[in->count] = tree;
    in->names[in->count] = strdup(name);
    in->analyses[in->count++] = NULL;
    return 0;
}
static char *with_suffix(const char *path, const char *suffix) {
    size_t n = strlen(path) + strlen(suffix) + 1;
    char *s = malloc(n);
    if (s)
        snprintf(s, n, "%s%s", path, suffix);
    return s;
}
static int write_artifacts(const char *output, const MoarVMProgramModel *model,
                           const MoarVMImage *image) {
    char *path = with_suffix(output, ".dump.txt");
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        free(path);
        return -1;
    }
    int rc = moarvm_dump_image(f, image);
    if (fclose(f))
        rc = -1;
    free(path);
    if (rc)
        return -1;
    path = with_suffix(output, ".model.txt");
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        free(path);
        return -1;
    }
    rc = moarvm_write_model(f, model);
    if (fclose(f))
        rc = -1;
    free(path);
    return rc;
}
static int write_graphs(const char *dir, const Inputs *in) {
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        perror(dir);
        return -1;
    }
    for (int i = 0; i < in->count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/unit-%d.ast.dot", i);
        char *path = with_suffix(dir, name);
        FILE *f = fopen(path, "w");
        if (!f) {
            perror(path);
            free(path);
            return -1;
        }
        ast_to_dot(f, in->original_trees ? in->original_trees[i] : in->trees[i]);
        int bad = ferror(f);
        if (fclose(f))
            bad = 1;
        free(path);
        if (bad)
            return -1;
        snprintf(name, sizeof(name), "/unit-%d.lowered.ast.dot", i);
        path = with_suffix(dir, name);
        f = fopen(path, "w");
        if (!f) {
            perror(path);
            free(path);
            return -1;
        }
        ast_to_dot(f, in->trees[i]);
        bad = ferror(f);
        if (fclose(f))
            bad = 1;
        free(path);
        if (bad)
            return -1;
        AnalysisResult *a = in->analyses[i];
        for (int j = 0; j < a->nfunctions; j++)
            if (a->functions[j]->has_body) {
                size_t size = strlen(dir) + strlen(a->functions[j]->name) + 50;
                path = malloc(size);
                snprintf(path, size, "%s/unit-%d-%s.cfg.dot", dir, i, a->functions[j]->name);
                f = fopen(path, "w");
                if (!f) {
                    perror(path);
                    free(path);
                    return -1;
                }
                write_function_dot(f, a->functions[j]);
                bad = ferror(f);
                if (fclose(f))
                    bad = 1;
                free(path);
                if (bad)
                    return -1;
            }
    }
    return 0;
}
int main(int argc, char **argv) {
    Inputs inputs = {0};
    MoarVMProgramModel model = {0};
    MoarVMImage image = {0};
    char *error = NULL;
    int rc = 1;
    const char *graphs = NULL, *output = NULL;
    int first = 1;
    while (first < argc && !strncmp(argv[first], "--", 2)) {
        if (!strcmp(argv[first], "--graphs") && first + 1 < argc) {
            graphs = argv[first + 1];
            first += 2;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[first]);
            return 2;
        }
    }
    if (argc - first < 2) {
        fprintf(stderr,
                "Usage: %s [--graphs directory] output.moarvm input1 [input2 ...]\n",
                argv[0]);
        return 2;
    }
    output = argv[first++];
    struct stat st;
    if (stat(output, &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "output is a directory: %s\n", output);
        return 2;
    }
    for (int i = first; i < argc; i++) {
        if (!strcmp(output, argv[i])) {
            fprintf(stderr, "output must not overwrite an input file\n");
            goto cleanup;
        }
        char *source = read_file(argv[i]);
        if (!source) {
            fprintf(stderr, "cannot read input file: %s\n", argv[i]);
            goto cleanup;
        }
        int status = add_input(&inputs, source, argv[i], 0);
        free(source);
        if (status)
            goto cleanup;
    }
    if (add_input(&inputs, moarvm_runtime_source(), "<builtin io>", 1))
        goto cleanup;
    if (graphs) {
        inputs.original_trees = calloc((size_t)inputs.count, sizeof(*inputs.original_trees));
        for (int i = 0; i < inputs.count; i++)
            inputs.original_trees[i] = ast_copy(inputs.trees[i]);
    }
    for (int i = 0; i < inputs.count; i++) {
        inputs.analyses[i] = build_cfg_from_ast(inputs.trees[i], inputs.names[i]);
        for (int j = 0; j < inputs.analyses[i]->nerrors; j++)
            fprintf(stderr, "%s: %s\n", inputs.names[i], inputs.analyses[i]->errors[j]);
        if (inputs.analyses[i]->nerrors)
            goto cleanup;
    }
    if (moarvm_build_program_model(inputs.analyses, inputs.count, &model)) {
        fprintf(stderr, "%s\n", model.error);
        goto cleanup;
    }
    int generation = moarvm_generate_image(&model, &image, &error);
    if (generation) {
        fprintf(stderr, "MoarVM generation failed: %s\n", error);
        goto cleanup;
    }
    if (graphs && write_graphs(graphs, &inputs))
        goto cleanup;
    if (write_artifacts(output, &model, &image)) {
        fprintf(stderr, "cannot write diagnostic artifacts\n");
        goto cleanup;
    }
    if (moarvm_image_write_file(&image, output)) {
        fprintf(stderr, "cannot write bytecode: %s\n", output);
        goto cleanup;
    }
    printf("Generated %s\nModel: %s.model.txt\nInstructions: %s.dump.txt\n", output, output,
           output);
    rc = 0;
cleanup:
    free(error);
    moarvm_image_free(&image);
    moarvm_free_program_model(&model);
    for (int i = 0; i < inputs.count; i++) {
        free_analysis(inputs.analyses[i]);
        ast_free(inputs.trees[i]);
        if (inputs.original_trees)
            ast_free(inputs.original_trees[i]);
        free(inputs.names[i]);
    }
    free(inputs.analyses);
    free(inputs.trees);
    free(inputs.original_trees);
    free(inputs.names);
    return rc;
}
