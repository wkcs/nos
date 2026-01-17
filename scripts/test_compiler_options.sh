#!/bin/bash
##############################################
# Copyright (C) 2023-2024 Nick Hu
#
# 编译器选项测试脚本
##############################################

set -e

echo "=== 编译器选项测试 ==="

# 测试 GCC 编译
echo ""
echo "测试 GCC 编译选项..."
make defconfig > /dev/null 2>&1
make check-compiler

echo "验证 GCC 配置文件..."
if grep -q "CONFIG_CC_IS_GCC=y" out/.config; then
    echo "✓ GCC 配置正确"
else
    echo "✗ GCC 配置错误"
fi

if grep -q "#define CONFIG_CC_IS_GCC 1" out/include/autoconf.h; then
    echo "✓ GCC 宏定义正确"
else
    echo "✗ GCC 宏定义错误"
fi

# 测试 Clang 编译（如果可用）
echo ""
echo "测试 Clang 编译选项..."
if make clang-test_config > /dev/null 2>&1; then
    make check-compiler
    
    echo "验证 Clang 配置文件..."
    if grep -q "CONFIG_CC_IS_CLANG=y" out/.config; then
        echo "✓ Clang 配置正确"
    else
        echo "✗ Clang 配置错误"
    fi
    
    if grep -q "#define CONFIG_CC_IS_CLANG 1" out/include/autoconf.h; then
        echo "✓ Clang 宏定义正确"
    else
        echo "✗ Clang 宏定义错误"
    fi
else
    echo "⚠ Clang 配置不可用，跳过测试"
fi

# 测试编译器特定选项的存在
echo ""
echo "验证编译器特定选项..."

# 检查 Makefile.config 中的编译器特定选项
if grep -q "CONFIG_CC_IS_CLANG" scripts/Makefile.config; then
    echo "✓ 编译器检测逻辑存在"
else
    echo "✗ 编译器检测逻辑缺失"
fi

if grep -q "Clang 特定选项" scripts/Makefile.config; then
    echo "✓ Clang 特定选项存在"
else
    echo "✗ Clang 特定选项缺失"
fi

if grep -q "GCC 特定选项" scripts/Makefile.config; then
    echo "✓ GCC 特定选项存在"
else
    echo "✗ GCC 特定选项缺失"
fi

# 检查架构特定配置
if [ -f "arch/arm/Makefile.config" ] && grep -q "ARCH_FLAGS" arch/arm/Makefile.config; then
    echo "✓ ARM 架构特定选项存在"
else
    echo "✗ ARM 架构特定选项缺失"
fi

if [ -f "arch/arm64/Makefile.config" ] && grep -q "ARCH_FLAGS" arch/arm64/Makefile.config; then
    echo "✓ ARM64 架构特定选项存在"
else
    echo "✗ ARM64 架构特定选项缺失"
fi

echo ""
echo "=== 编译器选项测试完成 ==="