/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Protected paths for the remote VFS export.
 */
#include <nos/nlk_caps.h>
#include <string.h>

static struct nlk_vfs_write_policy *write_policies;

static bool protected_path_denied(const char *path);

void nlk_cap_vfs_add_write_policy(struct nlk_vfs_write_policy *policy)
{
    if (policy == NULL || policy->paths == NULL || policy->count == 0) {
        return;
    }
    for (struct nlk_vfs_write_policy *entry = write_policies; entry != NULL; entry = entry->next) {
        if (entry == policy) {
            return;
        }
    }
    static struct nlk_vfs_write_guard guard = {.denies = protected_path_denied};

    policy->next = write_policies;
    write_policies = policy;
    nlk_cap_vfs_register_write_guard(&guard);
}

/* Use ASCII folding for the exported rules because FAT paths are case-insensitive. */
static bool path_prefix(const char *path, const char *prefix, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char a = path[i];
        unsigned char b = prefix[i];

        if (a >= 'A' && a <= 'Z') {
            a += 'a' - 'A';
        }
        if (b >= 'A' && b <= 'Z') {
            b += 'a' - 'A';
        }
        if (a == '\0' || a != b) {
            return false;
        }
    }
    return true;
}

static bool protected_path_denied(const char *path)
{
    char normalized[NLK_VFS_PATH_MAX + 2U];
    size_t used = 1;
    const char *p = path;
    /* Remote VFS operations resolve relative names from the root as well. */
    normalized[0] = '/';
    while (*p) {
        const char *start;
        size_t length;

        while (*p == '/') {
            p++;
        }
        start = p;
        while (*p && *p != '/') {
            p++;
        }
        length = p - start;
        if (!length || (length == 1 && start[0] == '.')) {
            continue;
        }
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            while (used > 1 && normalized[--used] != '/') {
            }
            continue;
        }
        if (used > 1) {
            normalized[used++] = '/';
        }
        if (length > sizeof(normalized) - used - 1) {
            return true;
        }
        memcpy(normalized + used, start, length);
        used += length;
    }
    normalized[used] = '\0';
    for (const struct nlk_vfs_write_policy *policy = write_policies;
        policy != NULL; policy = policy->next) {
        for (size_t i = 0; i < policy->count; i++) {
            const struct nlk_vfs_protected_path *rule = &policy->paths[i];
            size_t length = strlen(rule->path);

            if (path_prefix(normalized, rule->path, length) &&
                (normalized[length] == '\0' ||
                 (rule->subtree && (length == 1U || normalized[length] == '/')))) {
                return true;
            }
            /* Renaming or removing an ancestor also changes the protected path. */
            if (used == 1 || (path_prefix(rule->path, normalized, used) && rule->path[used] == '/')) {
                return true;
            }
        }
    }
    return false;
}
