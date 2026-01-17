#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
保存最小化配置（defconfig）
Copyright (C) 2023-2024 Nick Hu
"""

import os
import sys
from kconfig import KconfigParser


def main():
    if len(sys.argv) < 4:
        print("Usage: savedefconfig.py <root_dir> <config_file> <defconfig_file>")
        sys.exit(1)
        
    root_dir = sys.argv[1]
    config_file = sys.argv[2]
    defconfig_file = sys.argv[3]
    
    # 创建解析器
    parser = KconfigParser(root_dir)
    parser.parse_file('Kconfig')
    
    # 加载当前配置
    config = parser.load_config(config_file)
    
    # 创建最小配置（只保存非默认值）
    minimal_config = {}
    
    for name, value in config.items():
        symbol = parser.symbols.get(name)
        if not symbol:
            # 保留未知符号
            minimal_config[name] = value
            continue
            
        # 检查是否为默认值
        default = symbol.default
        if default:
            default = default.strip('"')
            if ' if ' in default:
                default = default.split(' if ')[0].strip()
                
        if value != default:
            minimal_config[name] = value
            
    # 保存最小配置
    with open(defconfig_file, 'w', encoding='utf-8') as f:
        f.write("# Minimal configuration\n\n")
        
        for name, value in sorted(minimal_config.items()):
            symbol = parser.symbols.get(name)
            if symbol and symbol.type == 'string':
                f.write(f'{name}="{value}"\n')
            else:
                f.write(f'{name}={value}\n')
                
    print(f"Minimal configuration saved to {defconfig_file}")


if __name__ == '__main__':
    main()
