#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
简化的 menuconfig 交互式配置界面
Copyright (C) 2023-2024 Nick Hu
"""

import os
import sys
from typing import Dict, List, Optional
from kconfig import KconfigParser, KconfigSymbol


class SimpleMenuConfig:
    """简化的配置界面（非 curses）"""
    
    def __init__(self, parser: KconfigParser, config: Dict[str, str]):
        self.parser = parser
        self.config = config
        
    def run(self):
        """运行交互界面"""
        print("=" * 60)
        print("NOS Kernel Configuration")
        print("=" * 60)
        
        while True:
            print("\n主菜单:")
            print("1. 架构配置")
            print("2. 内核配置")
            print("3. 内存管理")
            print("4. 驱动程序")
            print("5. 文件系统")
            print("6. 显示当前配置")
            print("s. 保存配置")
            print("q. 退出")
            
            choice = input("\n请选择 (1-6/s/q): ").strip().lower()
            
            if choice == '1':
                self.arch_menu()
            elif choice == '2':
                self.kernel_menu()
            elif choice == '3':
                self.memory_menu()
            elif choice == '4':
                self.driver_menu()
            elif choice == '5':
                self.fs_menu()
            elif choice == '6':
                self.show_config()
            elif choice == 's':
                return True
            elif choice == 'q':
                return False
            else:
                print("无效选择，请重试。")
                
    def arch_menu(self):
        """架构配置菜单"""
        print("\n架构配置:")
        
        # 架构选择
        current_arch = "ARM" if self.config.get('ARCH_ARM') == 'y' else "ARM64"
        print(f"当前架构: {current_arch}")
        
        print("1. ARM (32-bit)")
        print("2. ARM64 (64-bit)")
        print("b. 返回")
        
        choice = input("选择架构 (1-2/b): ").strip()
        
        if choice == '1':
            self.config['ARCH_ARM'] = 'y'
            self.config['ARCH_ARM64'] = 'n'
            print("已选择 ARM 架构")
        elif choice == '2':
            self.config['ARCH_ARM'] = 'n'
            self.config['ARCH_ARM64'] = 'y'
            print("已选择 ARM64 架构")
            
    def kernel_menu(self):
        """内核配置菜单"""
        print("\n内核配置:")
        
        options = [
            ('SHELL', 'Shell 支持'),
            ('DEVICE_TREE', '设备树支持'),
            ('MM_DEBUG', '内存管理调试'),
            ('SPINLOCK_DEBUG', '自旋锁调试')
        ]
        
        for i, (key, desc) in enumerate(options, 1):
            status = "启用" if self.config.get(key) == 'y' else "禁用"
            print(f"{i}. {desc}: {status}")
            
        print("b. 返回")
        
        choice = input("选择选项 (1-4/b): ").strip()
        
        if choice.isdigit() and 1 <= int(choice) <= len(options):
            key, desc = options[int(choice) - 1]
            current = self.config.get(key, 'n')
            new_value = 'n' if current == 'y' else 'y'
            self.config[key] = new_value
            status = "启用" if new_value == 'y' else "禁用"
            print(f"{desc} 已{status}")
            
    def memory_menu(self):
        """内存管理菜单"""
        print("\n内存管理配置:")
        
        # MMU 支持
        mmu_status = "启用" if self.config.get('MMU') == 'y' else "禁用"
        print(f"1. MMU 支持: {mmu_status}")
        
        # 页面大小
        page_size = self.config.get('PAGE_SIZE', '4096')
        print(f"2. 页面大小: {page_size}")
        
        # 最大阶数
        max_order = self.config.get('MAX_ORDER', '9')
        print(f"3. Buddy 最大阶数: {max_order}")
        
        print("b. 返回")
        
        choice = input("选择选项 (1-3/b): ").strip()
        
        if choice == '1':
            current = self.config.get('MMU', 'n')
            self.config['MMU'] = 'n' if current == 'y' else 'y'
            status = "启用" if self.config['MMU'] == 'y' else "禁用"
            print(f"MMU 支持已{status}")
        elif choice == '2':
            new_size = input(f"输入页面大小 (当前: {page_size}): ").strip()
            if new_size.isdigit():
                self.config['PAGE_SIZE'] = new_size
                print(f"页面大小设置为 {new_size}")
        elif choice == '3':
            new_order = input(f"输入最大阶数 (当前: {max_order}): ").strip()
            if new_order.isdigit() and 5 <= int(new_order) <= 15:
                self.config['MAX_ORDER'] = new_order
                print(f"最大阶数设置为 {new_order}")
                
    def driver_menu(self):
        """驱动程序菜单"""
        print("\n驱动程序配置:")
        
        options = [
            ('USB', 'USB 支持'),
            ('KEYBOARD', '键盘支持'),
            ('DISPLAY_SERVER', '显示服务器'),
            ('NK60_V2_LED', 'NK60 V2 LED 驱动'),
            ('NK60_V2_KEY', 'NK60 V2 键盘驱动')
        ]
        
        for i, (key, desc) in enumerate(options, 1):
            status = "启用" if self.config.get(key) == 'y' else "禁用"
            print(f"{i}. {desc}: {status}")
            
        print("b. 返回")
        
        choice = input("选择选项 (1-5/b): ").strip()
        
        if choice.isdigit() and 1 <= int(choice) <= len(options):
            key, desc = options[int(choice) - 1]
            current = self.config.get(key, 'n')
            new_value = 'n' if current == 'y' else 'y'
            self.config[key] = new_value
            status = "启用" if new_value == 'y' else "禁用"
            print(f"{desc} 已{status}")
            
    def fs_menu(self):
        """文件系统菜单"""
        print("\n文件系统配置:")
        
        options = [
            ('VFS', 'VFS 支持'),
            ('RAMFS', 'RAM 文件系统'),
            ('PROCFS', 'Proc 文件系统'),
            ('SYSFS', 'Sys 文件系统'),
            ('FATFS', 'FAT 文件系统')
        ]
        
        for i, (key, desc) in enumerate(options, 1):
            status = "启用" if self.config.get(key) == 'y' else "禁用"
            print(f"{i}. {desc}: {status}")
            
        print("b. 返回")
        
        choice = input("选择选项 (1-5/b): ").strip()
        
        if choice.isdigit() and 1 <= int(choice) <= len(options):
            key, desc = options[int(choice) - 1]
            current = self.config.get(key, 'n')
            new_value = 'n' if current == 'y' else 'y'
            self.config[key] = new_value
            status = "启用" if new_value == 'y' else "禁用"
            print(f"{desc} 已{status}")
            
    def show_config(self):
        """显示当前配置"""
        print("\n当前配置:")
        print("-" * 40)
        
        enabled_options = []
        for key, value in sorted(self.config.items()):
            if value == 'y':
                enabled_options.append(key)
                
        if enabled_options:
            for option in enabled_options:
                print(f"CONFIG_{option}=y")
        else:
            print("没有启用的选项")
            
        print("-" * 40)
        input("按 Enter 继续...")


def main():
    if len(sys.argv) < 3:
        print("Usage: menuconfig.py <root_dir> <config_file>")
        sys.exit(1)
        
    root_dir = sys.argv[1]
    config_file = sys.argv[2]
    
    # 创建解析器
    parser = KconfigParser(root_dir)
    parser.parse_file('Kconfig')
    
    # 加载配置
    config = parser.load_config(config_file)
    parser.apply_defaults(config)
    
    # 运行交互界面
    menu = SimpleMenuConfig(parser, config)
    
    try:
        save = menu.run()
        
        if save:
            # 应用 select 语句
            parser.apply_selects(config)
            
            # 保存配置
            parser.save_config(config, config_file)
            
            # 生成 autoconf.h
            output_dir = os.path.dirname(config_file)
            parser.generate_autoconf(config, os.path.join(output_dir, 'include', 'autoconf.h'))
            
            print("配置已保存")
        else:
            print("配置未保存")
    except KeyboardInterrupt:
        print("\n配置已取消")
        sys.exit(1)


if __name__ == '__main__':
    main()
