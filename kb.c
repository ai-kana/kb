#define KB_IMPLEMENTATION
#include "kb.h"

int
main()
{
    // Rebuilds and runs self if changed
    // Setting compiler to NULL defaults to cc
    kb_rebuild_self(NULL);

    char** files = NULL;
    size_t count = 0;
    kb_get_c_files(&files, &count);

    kb_free_c_files(files, count);

    // KB works by recording and submitting build commands to a buffer
    kb_cmd_buf buf;
    kb_create_buf(&buf);

    // Add a compile pass
    // All files in .files.names are built as objects
    //  using specified compiler and flags
    //
    // A compile pass is parrallelized
    kb_add_compilation_pass(buf, &(kb_compilation_pass_t) {
        .compiler = "cc",
        .build_dir = "obj",
        .flags = "-Wall",
        .files = {
            .names = (const char*[]) {
                "main.c"
            },
            .count = 1
        }
    });

    // All specified objects are linked during a linker pass
    kb_add_link_pass(buf, &(kb_link_pass_t) {
        .linker = "cc",
        .build_dir = ".",
        .output_name = "out",
        .flags = "-Wall",
        .files = {
            .names  = (const char*[]) {
                "obj/main.o"
            },
            .count = 1
        }
    });

    // Execute all recorded commands
    // Buffers can be submitted as many times as you wanted
    kb_submit_buf(buf);

    kb_cmd_buf prim;
    kb_create_buf(&prim);

    // Buffers can be recorded and execute by other buffers
    kb_add_secondary(prim, buf);

    // Free the buffer
    kb_destroy_buf(prim);
    kb_destroy_buf(buf);
}
