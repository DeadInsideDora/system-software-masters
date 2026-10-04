#ifndef MOARVM_DIRECT_H
#define MOARVM_DIRECT_H
#include "moarvm_model.h"
#include "moarvm_image.h"
int moarvm_generate_image(const MoarVMProgramModel *, MoarVMImage *, char **error);
int moarvm_generate_library_image(const MoarVMProgramModel *, MoarVMImage *, char **error);
int moarvm_generate_ffi_runtime_image(MoarVMImage *, char **error);
int moarvm_dump_image(FILE *, const MoarVMImage *);
const char *moarvm_runtime_source(void);
#endif
