/* Copyright (C) 2026 NOS contributors. Native VFS commands for MCU nshell. */
#include <stdio.h>
#include <string.h>
#include "nos/user.h"
#include "nos/esp_shell.h"

struct output {
    void (*emit)(void *, const char *, size_t);
    void *context;
};
static void text(struct output *out, const char *value) {
    out->emit(out->context, value, strlen(value));
}
static void error(struct output *out, const char *name, intptr_t rc) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%s: error=%ld\n", name, (long)rc);
    text(out, buffer);
}
static intptr_t finish(int fd, intptr_t rc) {
    int closed = nos_close(fd);
    return rc < 0 ? rc : closed;
}
static intptr_t list(struct output *out, const char *path) {
    int fd = nos_open(path, NOS_O_RDONLY | NOS_O_DIRECTORY);
    if (fd < 0)
        return fd;
    char name[NOS_PATH_MAX];
    intptr_t rc;
    for (unsigned i = 0;; i++) {
        rc = nos_readdir(fd, i, name, sizeof(name));
        if (rc <= 0)
            break;
        text(out, name);
        text(out, "\n");
    }
    return finish(fd, rc);
}
static intptr_t cat(struct output *out, const char *path) {
    int fd = nos_open(path, NOS_O_RDONLY);
    if (fd < 0)
        return fd;
    char buffer[128];
    intptr_t rc;
    while ((rc = nos_read_nowait(fd, buffer, sizeof(buffer))) > 0)
        out->emit(out->context, buffer, (size_t)rc);
    return finish(fd, rc);
}
static intptr_t write_file(const char *path, const char *data, unsigned append) {
    int fd = nos_open(path, NOS_O_WRONLY | NOS_O_CREAT | (append ? NOS_O_APPEND : NOS_O_TRUNC));
    if (fd < 0)
        return fd;
    size_t size = strlen(data);
    intptr_t rc = nos_write(fd, data, size);
    if (rc >= 0 && (size_t)rc != size)
        rc = -NOS_ENOSPC;
    return finish(fd, rc);
}
int nos_esp_shell_files(int count, char **words, void (*emit)(void *, const char *, size_t),
                        void *context) {
    struct output out = {emit, context};
    const char *name = words[0];
    int argc = -1;
    if (!strcmp(name, "pwd"))
        argc = 1;
    else if (!strcmp(name, "sync"))
        argc = 1;
    else if (!strcmp(name, "cd") || !strcmp(name, "ls"))
        argc = 0;
    else if (!strcmp(name, "cat") || !strcmp(name, "mkdir") || !strcmp(name, "rm") ||
             !strcmp(name, "rmdir") || !strcmp(name, "touch") || !strcmp(name, "stat") ||
             !strcmp(name, "mount"))
        argc = 2;
    else if (!strcmp(name, "write") || !strcmp(name, "append") || !strcmp(name, "mv"))
        argc = 3;
    if (argc < 0)
        return 0;
    if ((argc && count != argc) || (!argc && count > 2)) {
        text(&out, "files: invalid argument count\n");
        return 1;
    }
    intptr_t rc = 0;
    if (!strcmp(name, "pwd")) {
        char path[NOS_PATH_MAX];
        rc = nos_getcwd(path, sizeof(path));
        if (rc >= 0) {
            text(&out, path);
            text(&out, "\n");
        }
    } else if (!strcmp(name, "mount"))
        rc = nos_mount(words[1], 1);
    else if (!strcmp(name, "sync"))
        rc = nos_sync();
    else if (!strcmp(name, "cd"))
        rc = nos_chdir(count == 2 ? words[1] : "/");
    else if (!strcmp(name, "ls"))
        rc = list(&out, count == 2 ? words[1] : ".");
    else if (!strcmp(name, "cat"))
        rc = cat(&out, words[1]);
    else if (!strcmp(name, "write") || !strcmp(name, "append"))
        rc = write_file(words[1], words[2], !strcmp(name, "append"));
    else if (!strcmp(name, "mkdir"))
        rc = nos_mkdir(words[1]);
    else if (!strcmp(name, "rm"))
        rc = nos_unlink(words[1]);
    else if (!strcmp(name, "rmdir"))
        rc = nos_rmdir(words[1]);
    else if (!strcmp(name, "mv"))
        rc = nos_rename(words[1], words[2]);
    else {
        int fd = nos_open(words[1], NOS_O_RDONLY | (!strcmp(name, "touch") ? NOS_O_CREAT : 0));
        rc = fd;
        if (fd >= 0) {
            if (!strcmp(name, "stat")) {
                struct nos_stat st;
                rc = nos_stat_fd(fd, &st);
                if (!rc) {
                    char buffer[96];
                    snprintf(buffer, sizeof(buffer), "size=%lu mode=%lo\n", (unsigned long)st.size,
                             (unsigned long)st.mode);
                    text(&out, buffer);
                }
            }
            rc = finish(fd, rc);
        }
    }
    if (rc < 0)
        error(&out, name, rc);
    else if (strcmp(name, "pwd") && strcmp(name, "cat") && strcmp(name, "ls") &&
             strcmp(name, "stat"))
        text(&out, "ok\n");
    return 1;
}
