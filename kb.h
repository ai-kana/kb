#ifndef __KB_H
#define __KB_H

#define __need_size_t
#include <stddef.h>

struct kb_cmd_buf_phony;
typedef struct kb_cmd_buf_phony* kb_cmd_buf;


typedef struct kb_files {
    char** names;
    size_t count;
} kb_files_t;

typedef struct kb_compilation_pass {
    kb_files_t files;
    const char* flags;
    const char* compiler;
    const char* build_dir;
} kb_compilation_pass_t;

typedef struct kb_link_pass {
    kb_files_t files;
    const char* flags;
    const char* linker;
    const char* build_dir;
    const char* output_name;
} kb_link_pass_t;

void kb_add_compilation_pass(kb_cmd_buf buf, kb_compilation_pass_t* pass);

void kb_add_link_pass(kb_cmd_buf buf, kb_link_pass_t* pass);

void kb_add_secondary(kb_cmd_buf buf, kb_cmd_buf secondary);

void kb_add_change_dir(kb_cmd_buf buf, const char* dir);

void kb_create_buf(kb_cmd_buf* buf);
void kb_destroy_buf(kb_cmd_buf buf);

void kb_submit_buf(kb_cmd_buf buf);

void kb_rebuild_self(const char* compiler);
void kb_get_c_files(kb_files_t* files, const char* path);
void kb_get_o_files(kb_files_t* files, const char* path);
void kb_free_files(kb_files_t* files);

//#define KB_IMPLEMENTATION
#ifdef KB_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <dirent.h>
#include <unistd.h>

typedef enum kb_cmd_type {
    kb_cmd_type_compilation_pass,
    kb_cmd_type_link_pass,
    kb_cmd_type_secondary,
    kb_cmd_type_change_dir,
} kb_cmd_type_t;

typedef struct kb_cmd_secondary {
    kb_cmd_buf buf;
} kb_cmd_secondary_t;

typedef struct kb_cmd_compilation_pass {
    kb_compilation_pass_t pass;
} kb_cmd_compilation_pass_t;

typedef struct kb_cmd_link_pass {
    kb_link_pass_t pass;
} kb_cmd_link_pass_t;

typedef struct kb_cmd {
    kb_cmd_type_t type;
    union {
        kb_cmd_compilation_pass_t compilation_pass;
        kb_cmd_link_pass_t link_pass;
        kb_cmd_secondary_t secondary;
        const char* change_dir;
    } command;
} kb_cmd_t;

typedef struct kb_cmds {
    kb_cmd_t* commands;
    size_t count;
    size_t capacity;
} kb_cmds_t;

void 
kb_create_buf(kb_cmd_buf* buf) 
{
    // initial commands allocated
    const size_t capacity = 8;
    kb_cmds_t* state = (kb_cmds_t*)calloc(1, sizeof(kb_cmds_t));
    state->commands = (kb_cmd_t*)malloc(sizeof(kb_cmd_t) * capacity);
    state->count = 0;
    state->capacity = capacity;

    *buf = (kb_cmd_buf)state;
}

void
kb_destroy_buf(kb_cmd_buf buf) 
{
    free(buf);
}

void
__kb_buf_append(kb_cmds_t* buf, kb_cmd_t* cmd)
{
    if (buf->count >= buf->capacity) {
        buf->capacity <<= 1;
        buf->commands = (kb_cmd_t*)realloc(buf->commands, buf->capacity * sizeof(kb_cmd_t));
    }

    memcpy(buf->commands + buf->count, cmd, sizeof(kb_cmd_t));
    buf->count += 1;
}

void 
kb_add_compilation_pass(kb_cmd_buf buf, kb_compilation_pass_t* pass)
{
    kb_cmd_t cmd;
    cmd.type = kb_cmd_type_compilation_pass;
    cmd.command.compilation_pass.pass = *pass;
    __kb_buf_append((kb_cmds_t*)buf, &cmd);
}

void 
kb_add_link_pass(kb_cmd_buf buf, kb_link_pass_t* pass)
{
    kb_cmd_t cmd;
    cmd.type = kb_cmd_type_link_pass;
    cmd.command.link_pass.pass = *pass;
    __kb_buf_append((kb_cmds_t*)buf, &cmd);
}

void
kb_add_change_dir(kb_cmd_buf buf, const char* dir)
{
    kb_cmd_t cmd;
    cmd.type = kb_cmd_type_change_dir;
    cmd.command.change_dir = dir;
    __kb_buf_append((kb_cmds_t*)buf, &cmd);
}

void 
kb_add_secondary(kb_cmd_buf buf, kb_cmd_buf secondary)
{
    kb_cmd_t cmd;
    cmd.type = kb_cmd_type_secondary;
    cmd.command.secondary.buf = secondary;
    __kb_buf_append((kb_cmds_t*)buf, &cmd);
}

typedef struct kb_build_thread_state {
    pthread_mutex_t mutex;
    char** commands;
    size_t current;
    size_t count;
} kb_build_thread_state_t;

static void*
kb_thread_start(void* arg)
{
    kb_build_thread_state_t* state = (kb_build_thread_state_t*)arg;
    while (1)
    {
        pthread_mutex_lock(&state->mutex);
        if (state->current >= state->count) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        const char* cmd = state->commands[state->current];
        state->current += 1;
        if (cmd == NULL) {
            pthread_mutex_unlock(&state->mutex);
            continue;
        }

        pthread_mutex_unlock(&state->mutex);

        puts(cmd);
        system(cmd);
    }

    return NULL;
}

static bool
needs_recompile(const char* c_file, const char* o_file)
{
    struct stat c_stat;
    if (stat(c_file, &c_stat)) {
        return false;
    }

    struct stat o_stat;
    if (stat(o_file, &o_stat)) {
        return true;
    }

    return c_stat.st_mtime - o_stat.st_mtime > 0;
}

#define BUILD_THREAD_COUNT 4
static void
kb_do_compilation_pass(const kb_compilation_pass_t* pass)
{
    mkdir(pass->build_dir, 0755);
    const char* first_pass_format = "%s %s %s";
    const char* second_pass_format = "-c -o %s %s";

    const size_t buf_size = 1024 + 1024 + 1024;
    char* first_pass_cmd_buf = (char*)malloc(buf_size);
    char* second_pass_cmd_buf = first_pass_cmd_buf + 1024;
    char* name_swap = second_pass_cmd_buf + 1024;

    char** commands = (char**)malloc(pass->files.count * sizeof(char*));

    snprintf(first_pass_cmd_buf, 1024, first_pass_format, pass->compiler, pass->flags, second_pass_format);
    for (size_t i = 0; i < pass->files.count; i++) {
        strncpy(name_swap, pass->files.names[i], 1024);
        snprintf(name_swap, 1024, "%s/%s", pass->build_dir, pass->files.names[i]);
        name_swap[strlen(name_swap) - 1] = 'o';

        if (!needs_recompile(pass->files.names[i], name_swap)) {
            commands[i] = NULL;
            continue;
        }

        snprintf(second_pass_cmd_buf, 1024, first_pass_cmd_buf, name_swap, pass->files.names[i]);
        const size_t len = strlen(second_pass_cmd_buf) + 1;
        commands[i] = (char*)malloc(len);
        memcpy(commands[i], second_pass_cmd_buf, len);
    }

    kb_build_thread_state_t* state = (kb_build_thread_state_t*)malloc(sizeof(kb_build_thread_state_t));
    state->commands = commands;
    state->current = 0;
    state->count = pass->files.count;
    pthread_mutex_init(&state->mutex, NULL);

    pthread_t threads[BUILD_THREAD_COUNT];
    size_t thread_count = BUILD_THREAD_COUNT; 
    if (pass->files.count < thread_count) {
        thread_count = pass->files.count;
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_create(&threads[i], NULL, &kb_thread_start, state);
    }

    for (size_t i = 0; i < thread_count; i++) {
        void* join;
        pthread_join(threads[i], &join);
    }
    pthread_mutex_destroy(&state->mutex);

    free(state);
    for (size_t i = 0; i < pass->files.count; i++) {;
        free(commands[i]);
    }

    free(first_pass_cmd_buf);
}

static void
kb_do_link_pass(const kb_link_pass_t* pass)
{
    mkdir(pass->build_dir, 0755);
    const char* first_pass_format = "%s %s %s";
    const char* second_pass_format = "-o %s/%s %s";

    const size_t buf_size = 1024 + 1024 + 1024;
    char* first_pass_cmd_buf = (char*)malloc(buf_size);
    char* second_pass_cmd_buf = first_pass_cmd_buf + 1024;
    char* files = second_pass_cmd_buf + 1024;
    size_t files_off = 0;

    snprintf(first_pass_cmd_buf, 1024, first_pass_format, pass->linker, pass->flags, second_pass_format);

    for (size_t i = 0; i < pass->files.count; i++) {
        const size_t len = strlen(pass->files.names[i]);
        memcpy(files + files_off, pass->files.names[i], len);
        files_off += len;
        files[files_off] = ' ';
        files_off++;
    }
    files[files_off] = '\0';

    snprintf(second_pass_cmd_buf, 1024, first_pass_cmd_buf, pass->build_dir, pass->output_name, files);
    puts(second_pass_cmd_buf);
    system(second_pass_cmd_buf);

    free(first_pass_cmd_buf);
}

void
kb_submit_buf(kb_cmd_buf buf)
{
    kb_cmds_t* cmds = (kb_cmds_t*)buf;
    for (size_t i = 0; i < cmds->count; i++) {
        const kb_cmd_t* cmd = &cmds->commands[i];
        switch (cmd->type) {
            case kb_cmd_type_compilation_pass:
                kb_do_compilation_pass(&cmd->command.compilation_pass.pass);
                break;
            case kb_cmd_type_link_pass:
                kb_do_link_pass(&cmd->command.link_pass.pass);
                break;
            case kb_cmd_type_secondary:
                kb_submit_buf(cmd->command.secondary.buf);
                break;
            case kb_cmd_type_change_dir:
                chdir(cmd->command.change_dir);
                break;
            default:
                break;
        }
    }
}

void
kb_rebuild_self(const char* compiler)
{
    const char* self = "kb.c";
    const char* bin = "kb";
    const char* cmd = "%s -o kb kb.c";

    if (!needs_recompile(self, bin)) {
        puts("Recompile not needed");
        return;
    }

    if (compiler == NULL) {
        compiler = "cc";
    }

    char buf[512];
    snprintf(buf, 512, cmd, compiler);
    puts("Recompiling self");
    puts(buf);
    system(buf);
    system("./kb");

    exit(0);
}

void 
kb_get_c_files(kb_files_t* files, const char* path)
{
    files->names = NULL;
    if (path == NULL) {
        path = ".";
    }
    DIR* dir = opendir(path);
    if (!dir) {
        return;
    }

    size_t capacity = 8;
    char** data = (char**)malloc(sizeof(char*) * 8);

    struct dirent* ent;
    files->count = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG) {
            continue;
        }

        const size_t len = strlen(ent->d_name);
        if (len < 3) {
            continue;
        }

        if (ent->d_name[len - 2] != '.' || ent->d_name[len - 1] != 'c') {
            continue;
        }

        if (ent->d_name[0] == 'k' && ent->d_name[1] == 'b') {
            continue;
        }

        char* buf = (char*)malloc(len + 1);
        memcpy(buf, ent->d_name, len + 1);
        if (files->count >= capacity) {
            capacity <<= 1;
            data = (char**)realloc(data, sizeof(char*) * capacity);
        }
        data[files->count] = buf;

        files->count += 1;
    }
    files->names = data;

    closedir(dir);
}

void 
kb_get_o_files(kb_files_t* files, const char* path)
{
    files->names = NULL;
    if (path == NULL) {
        path = ".";
    }
    DIR* dir = opendir(path);
    if (!dir) {
        return;
    }

    size_t capacity = 8;
    char** data = (char**)malloc(sizeof(char*) * 8);

    struct dirent* ent;
    files->count = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG) {
            continue;
        }

        const size_t len = strlen(ent->d_name);
        if (len < 3) {
            continue;
        }

        if (ent->d_name[len - 2] != '.' || ent->d_name[len - 1] != 'o') {
            continue;
        }

        if (ent->d_name[0] == 'k' && ent->d_name[1] == 'b') {
            continue;
        }

        char* buf = (char*)malloc(len + 1);
        memcpy(buf, ent->d_name, len + 1);
        if (files->count >= capacity) {
            capacity <<= 1;
            data = (char**)realloc(data, sizeof(char*) * capacity);
        }
        data[files->count] = buf;

        files->count += 1;
    }
    files->names = data;

    closedir(dir);
}

void
kb_free_files(kb_files_t* files)
{
    for (size_t i = 0; i < files->count; i++) {
        free(files->names[i]);
    }

    free(files->names);
}

#endif

#endif
