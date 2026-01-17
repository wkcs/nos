/**
 * Copyright (C) 2023-2024 Nick Hu
 * 
 * 内核测试主函数
 */

#include <kernel/test.h>
#include <kernel/printk.h>

/**
 * 运行所有属性测试
 */
int run_all_property_tests(void)
{
    int total_errors = 0;
    
    pr_info("=== 开始运行内核属性测试 ===\n");
    
    /* 运行编译器切换机制测试 */
    if (compiler_switch_property_test() != 0) {
        total_errors++;
    }
    
    /* 这里可以添加更多测试 */
    
    pr_info("=== 属性测试完成 ===\n");
    
    if (total_errors == 0) {
        pr_info("所有属性测试通过!\n");
        return 0;
    } else {
        pr_err("有 %d 个属性测试失败\n", total_errors);
        return -1;
    }
}