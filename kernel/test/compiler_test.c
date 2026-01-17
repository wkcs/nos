/**
 * Copyright (C) 2023-2024 Nick Hu
 * 
 * 编译器切换机制属性测试
 * 
 * 验证需求: 2.4 - 编译器切换机制
 */

#include <kernel/types.h>
#include <kernel/printk.h>

/* 编译器特定的功能测试 */

/**
 * 测试编译器内置宏定义
 */
static int test_compiler_macros(void)
{
    int errors = 0;
    
    pr_info("测试编译器宏定义...\n");
    
#ifdef __GNUC__
    pr_info("检测到 GCC 编译器，版本: %d.%d.%d\n", 
            __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
    
    /* 验证 GCC 特定功能 */
    #ifndef __GNUC_GNU_INLINE__
        pr_err("GCC 缺少 GNU inline 支持\n");
        errors++;
    #endif
    
#elif defined(__clang__)
    pr_info("检测到 Clang 编译器，版本: %d.%d.%d\n",
            __clang_major__, __clang_minor__, __clang_patchlevel__);
    
    /* 验证 Clang 特定功能 */
    #ifndef __has_builtin
        pr_err("Clang 缺少 __has_builtin 支持\n");
        errors++;
    #endif
    
#else
    pr_err("未知编译器\n");
    errors++;
#endif

    return errors;
}

/**
 * 测试内联汇编支持
 */
static int test_inline_assembly(void)
{
    int errors = 0;
    volatile int test_val = 42;
    int result;
    
    pr_info("测试内联汇编...\n");
    
    /* 简单的内联汇编测试 */
    __asm__ volatile (
        "mov %0, %1"
        : "=r" (result)
        : "r" (test_val)
        : "memory"
    );
    
    if (result != test_val) {
        pr_err("内联汇编测试失败: 期望 %d, 得到 %d\n", test_val, result);
        errors++;
    } else {
        pr_info("内联汇编测试通过\n");
    }
    
    return errors;
}

/**
 * 测试编译器属性支持
 */
static int test_compiler_attributes(void)
{
    int errors = 0;
    
    pr_info("测试编译器属性...\n");
    
    /* 测试 __attribute__ 支持 */
    struct __attribute__((packed)) test_struct {
        char a;
        int b;
    };
    
    if (sizeof(struct test_struct) != 5) {
        pr_err("packed 属性测试失败: 期望大小 5, 得到 %zu\n", 
               sizeof(struct test_struct));
        errors++;
    } else {
        pr_info("packed 属性测试通过\n");
    }
    
    return errors;
}

/**
 * 测试原子操作支持
 */
static int test_atomic_operations(void)
{
    int errors = 0;
    volatile int atomic_var = 0;
    
    pr_info("测试原子操作...\n");
    
    /* 测试原子加法 */
    int old_val = __sync_fetch_and_add(&atomic_var, 1);
    if (old_val != 0 || atomic_var != 1) {
        pr_err("原子加法测试失败\n");
        errors++;
    } else {
        pr_info("原子加法测试通过\n");
    }
    
    /* 测试比较交换 */
    int expected = 1;
    int new_val = 2;
    if (!__sync_bool_compare_and_swap(&atomic_var, expected, new_val)) {
        pr_err("原子比较交换测试失败\n");
        errors++;
    } else if (atomic_var != 2) {
        pr_err("原子比较交换结果错误\n");
        errors++;
    } else {
        pr_info("原子比较交换测试通过\n");
    }
    
    return errors;
}

/**
 * 编译器切换机制属性测试主函数
 */
int compiler_switch_property_test(void)
{
    int total_errors = 0;
    
    pr_info("=== 编译器切换机制属性测试 ===\n");
    
    total_errors += test_compiler_macros();
    total_errors += test_inline_assembly();
    total_errors += test_compiler_attributes();
    total_errors += test_atomic_operations();
    
    if (total_errors == 0) {
        pr_info("编译器切换机制属性测试: 通过\n");
        return 0;
    } else {
        pr_err("编译器切换机制属性测试: 失败 (%d 个错误)\n", total_errors);
        return -1;
    }
}