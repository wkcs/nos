/**
 * Copyright (C) 2023-2024 Nick Hu
 * 
 * 内核测试框架头文件
 */

#ifndef __KERNEL_TEST_H__
#define __KERNEL_TEST_H__

#include <kernel/types.h>

/* 测试函数声明 */

/**
 * 编译器切换机制属性测试
 * 验证需求: 2.4
 */
int compiler_switch_property_test(void);

/**
 * 运行所有属性测试
 */
int run_all_property_tests(void);

#endif /* __KERNEL_TEST_H__ */