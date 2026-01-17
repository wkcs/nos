#!/bin/bash
##############################################
# Copyright (C) 2023-2024 Nick Hu
#
# 编译器切换测试脚本
##############################################

set -e

echo "=== 编译器切换机制测试 ==="

# 测试 GCC 编译器
echo "测试 GCC 编译器..."
make defconfig > /dev/null 2>&1
make check-compiler

# 检查配置文件中的编译器设置
if grep -q "CONFIG_CC_IS_GCC=y" out/.config; then
    echo "✓ GCC 配置正确"
else
    echo "✗ GCC 配置错误"
    exit 1
fi

# 检查 autoconf.h 中的编译器宏
if grep -q "#define CONFIG_CC_IS_GCC 1" out/include/autoconf.h; then
    echo "✓ GCC 宏定义正确"
else
    echo "✗ GCC 宏定义错误"
    exit 1
fi

echo ""

# 测试 Clang 编译器（如果可用）
echo "测试 Clang 编译器..."
if make clang-test_config > /dev/null 2>&1; then
    make check-compiler
    
    # 检查配置文件中的编译器设置
    if grep -q "CONFIG_CC_IS_CLANG=y" out/.config; then
        echo "✓ Clang 配置正确"
    else
        echo "✗ Clang 配置错误"
        exit 1
    fi
    
    # 检查 autoconf.h 中的编译器宏
    if grep -q "#define CONFIG_CC_IS_CLANG 1" out/include/autoconf.h; then
        echo "✓ Clang 宏定义正确"
    else
        echo "✗ Clang 宏定义错误"
        exit 1
    fi
else
    echo "⚠ Clang 配置不可用，跳过测试"
fi

echo ""
echo "=== 编译器切换机制测试通过 ==="