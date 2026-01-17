#!/usr/bin/env python3
"""
编译器检测和验证脚本
Copyright (C) 2023-2024 Nick Hu

检测可用的编译器并验证其版本和功能。
"""

import subprocess
import sys
import os
import re

def run_command(cmd):
    """运行命令并返回输出"""
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)

def check_gcc(prefix=""):
    """检查 GCC 编译器"""
    gcc_cmd = f"{prefix}gcc"
    
    # 检查编译器是否存在
    ret, stdout, stderr = run_command(f"which {gcc_cmd}")
    if ret != 0:
        return False, f"GCC 编译器 {gcc_cmd} 未找到"
    
    # 检查版本
    ret, stdout, stderr = run_command(f"{gcc_cmd} --version")
    if ret != 0:
        return False, f"无法获取 GCC 版本信息"
    
    # 解析版本号
    version_match = re.search(r'gcc.*?(\d+)\.(\d+)\.(\d+)', stdout.lower())
    if version_match:
        major, minor, patch = map(int, version_match.groups())
        version_str = f"{major}.{minor}.{patch}"
        
        # 检查最低版本要求 (GCC 7.0+)
        if major < 7:
            return False, f"GCC 版本 {version_str} 过低，需要 7.0 或更高版本"
        
        return True, f"GCC {version_str} 可用"
    
    return False, "无法解析 GCC 版本信息"

def check_clang(prefix="", target_arch=None):
    """检查 Clang 编译器"""
    # 首先尝试带前缀的 clang
    clang_cmd = f"{prefix}clang"
    ret, stdout, stderr = run_command(f"which {clang_cmd}")
    
    if ret != 0:
        # 尝试系统 clang
        clang_cmd = "clang"
        ret, stdout, stderr = run_command(f"which {clang_cmd}")
        if ret != 0:
            return False, f"Clang 编译器未找到"
    
    # 检查版本
    ret, stdout, stderr = run_command(f"{clang_cmd} --version")
    if ret != 0:
        return False, f"无法获取 Clang 版本信息"
    
    # 解析版本号
    version_match = re.search(r'clang version (\d+)\.(\d+)\.(\d+)', stdout)
    if version_match:
        major, minor, patch = map(int, version_match.groups())
        version_str = f"{major}.{minor}.{patch}"
        
        # 检查最低版本要求 (Clang 10.0+)
        if major < 10:
            return False, f"Clang 版本 {version_str} 过低，需要 10.0 或更高版本"
        
        # 检查目标架构支持
        if target_arch:
            target_map = {
                'arm': 'arm-none-eabi',
                'arm64': 'aarch64-none-elf'
            }
            target = target_map.get(target_arch)
            if target:
                ret, stdout, stderr = run_command(f"{clang_cmd} --target={target} -v")
                if ret != 0:
                    return False, f"Clang 不支持目标架构 {target}"
        
        return True, f"Clang {version_str} 可用"
    
    return False, "无法解析 Clang 版本信息"

def check_binutils(prefix=""):
    """检查 binutils 工具"""
    tools = ['ld', 'ar', 'as', 'objcopy', 'objdump', 'size']
    missing_tools = []
    
    for tool in tools:
        tool_cmd = f"{prefix}{tool}"
        ret, stdout, stderr = run_command(f"which {tool_cmd}")
        if ret != 0:
            missing_tools.append(tool_cmd)
    
    if missing_tools:
        return False, f"缺少工具: {', '.join(missing_tools)}"
    
    return True, "所有 binutils 工具可用"

def main():
    """主函数"""
    if len(sys.argv) < 3:
        print("用法: check_compiler.py <compiler> <arch> [prefix]")
        print("  compiler: gcc 或 clang")
        print("  arch: arm 或 arm64")
        print("  prefix: 工具链前缀 (可选)")
        sys.exit(1)
    
    compiler = sys.argv[1]
    arch = sys.argv[2]
    prefix = sys.argv[3] if len(sys.argv) > 3 else ""
    
    print(f"检查编译器: {compiler}")
    print(f"目标架构: {arch}")
    print(f"工具链前缀: {prefix}")
    print("-" * 40)
    
    # 检查编译器
    if compiler == "gcc":
        success, message = check_gcc(prefix)
    elif compiler == "clang":
        success, message = check_clang(prefix, arch)
    else:
        print(f"不支持的编译器: {compiler}")
        sys.exit(1)
    
    print(f"编译器检查: {message}")
    if not success:
        sys.exit(1)
    
    # 检查 binutils
    success, message = check_binutils(prefix)
    print(f"工具链检查: {message}")
    if not success:
        sys.exit(1)
    
    print("编译器检查通过!")
    return 0

if __name__ == "__main__":
    main()