/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * nShell 扩展命令注册与查找机制主机单元测试
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <nos/shell_cmd.h>

/* 测试命令执行函数 1 */
static int test_cmd1_handler(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return 101;
}

/* 测试命令执行函数 2 */
static int test_cmd2_handler(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return 202;
}

/* 静态测试命令描述符 */
static struct nos_shell_cmd g_cmd1 = {
    .name = "test_cmd1",
    .desc = "Test command 1 description",
    .func = test_cmd1_handler,
    .next = NULL,
};

static struct nos_shell_cmd g_cmd2 = {
    .name = "test_cmd2",
    .desc = "Test command 2 description",
    .func = test_cmd2_handler,
    .next = NULL,
};

static struct nos_shell_cmd g_cmd1_dup = {
    .name = "test_cmd1",
    .desc = "Duplicate test command 1",
    .func = test_cmd1_handler,
    .next = NULL,
};

int main(void)
{
    const struct nos_shell_cmd *found;
    const struct nos_shell_cmd *curr;
    int count;
    int ret;

    /* 1. 测试非法参数注册 */
    assert(nos_shell_register_cmd(NULL) == -EINVAL);

    struct nos_shell_cmd invalid_name = {
        .name = NULL,
        .desc = "No name",
        .func = test_cmd1_handler,
        .next = NULL,
    };
    assert(nos_shell_register_cmd(&invalid_name) == -EINVAL);

    struct nos_shell_cmd invalid_func = {
        .name = "invalid_func",
        .desc = "No func",
        .func = NULL,
        .next = NULL,
    };
    assert(nos_shell_register_cmd(&invalid_func) == -EINVAL);

    /* 2. 正常注册命令 1 */
    ret = nos_shell_register_cmd(&g_cmd1);
    assert(ret == 0);

    /* 3. 重复注册相同指针必须被拦截，防止链表自环 */
    ret = nos_shell_register_cmd(&g_cmd1);
    assert(ret == -EEXIST);
    assert(g_cmd1.next != &g_cmd1);

    /* 4. 注册同名但不同指针的命令必须被拦截，防止重名冲突 */
    ret = nos_shell_register_cmd(&g_cmd1_dup);
    assert(ret == -EEXIST);

    /* 5. 注册命令 2 */
    ret = nos_shell_register_cmd(&g_cmd2);
    assert(ret == 0);

    /* 6. 测试查找表命中 */
    found = nos_shell_find_cmd("test_cmd1");
    assert(found != NULL);
    assert(strcmp(found->name, "test_cmd1") == 0);
    assert(found->func != NULL);
    assert(found->func(0, NULL) == 101);

    found = nos_shell_find_cmd("test_cmd2");
    assert(found != NULL);
    assert(strcmp(found->name, "test_cmd2") == 0);
    assert(found->func != NULL);
    assert(found->func(0, NULL) == 202);

    /* 查找不存在的命令必须返回 NULL */
    found = nos_shell_find_cmd("non_existent_command");
    assert(found == NULL);

    /* 7. 遍历扩展命令列表并检查命令数量与内容 */
    count = 0;
    bool found_cmd1 = false;
    bool found_cmd2 = false;
    for (curr = nos_shell_get_ext_cmds(); curr != NULL; curr = curr->next) {
        count++;
        if (strcmp(curr->name, "test_cmd1") == 0) {
            found_cmd1 = true;
        } else if (strcmp(curr->name, "test_cmd2") == 0) {
            found_cmd2 = true;
        }
        /* 防止环路导致的死循环 */
        assert(count <= 100);
    }
    assert(count == 2);
    assert(found_cmd1 && found_cmd2);

    printf("PASS: test_shell_ext_cmd completed successfully\n");
    return 0;
}
