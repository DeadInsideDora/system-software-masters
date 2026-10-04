#include "moarvm_direct.h"
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s runtime.moarvm runtime.h\n", argv[0]);
        return 2;
    }
    MoarVMImage image = {0};
    char *error = NULL;
    int status = moarvm_generate_ffi_runtime_image(&image, &error);
    if (status) {
        fprintf(stderr, "%s\n", error);
        free(error);
        return 1;
    }
    status = moarvm_image_write_file(&image, argv[1]);
    char *listing = malloc(strlen(argv[1]) + 10);
    if (!listing) {
        moarvm_image_free(&image);
        return 1;
    }
    sprintf(listing, "%s.dump.txt", argv[1]);
    FILE *dump = fopen(listing, "w");
    free(listing);
    if (!dump)
        status = 1;
    else {
        if (moarvm_dump_image(dump, &image))
            status = 1;
        if (fclose(dump))
            status = 1;
    }
    moarvm_image_free(&image);
    if (status)
        return 1;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "w");
    if (!in || !out) {
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        return 1;
    }
    fputs("static const unsigned char "
          "spo_ffi_runtime[] = {\n",
          out);
    unsigned char bytes[4096];
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), in)) > 0) {
        for (size_t i = 0; i < count; i++)
            fprintf(out, "0x%02x,%s", bytes[i], i % 16 == 15 ? "\n" : "");
    }
    fputs("\n};\n", out);
    status = ferror(in) || ferror(out);
    if (fclose(in))
        status = 1;
    if (fclose(out))
        status = 1;
    return status;
}
