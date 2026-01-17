#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
改进的 menuconfig 交互式配置界面，支持层级菜单和完整功能
Copyright (C) 2023-2024 Nick Hu
"""

import os
import sys
from typing import Dict, List, Optional, Union
from kconfig_improved import KconfigParser, KconfigSymbol, KconfigChoice, KconfigMenu


class MenuConfigUI:
    """改进的配置界面"""
    
    def __init__(self, parser: KconfigParser, config: Dict[str, str]):
        self.parser = parser
        self.config = config
        self.menu_stack = []
        self.current_menu = None
        
    def run(self):
        """运行交互界面"""
        print("=" * 60)
        print("NOS Kernel Configuration")
        print("=" * 60)
        
        # 构建主菜单
        self.build_main_menu()
        
        try:
            return self.show_menu(self.current_menu)
        except KeyboardInterrupt:
            print("\n配置已取消")
            return False
            
    def build_main_menu(self):
        """构建主菜单"""
        main_menu = KconfigMenu("Main Menu")
        
        # 添加架构菜单
        arch_menu = KconfigMenu("Architecture")
        for name, symbol in self.parser.symbols.items():
            if name.startswith('ARCH_') and symbol.prompt:
                arch_menu.add_item(symbol)
        main_menu.add_item(arch_menu)
        
        # 添加其他现有菜单
        for menu in self.parser.menus:
            main_menu.add_item(menu)
            
        # 添加未分类的符号
        uncategorized = KconfigMenu("Other Options")
        for name, symbol in self.parser.symbols.items():
            if symbol.prompt and not self._is_symbol_in_menus(symbol):
                uncategorized.add_item(symbol)
        if uncategorized.items:
            main_menu.add_item(uncategorized)
            
        self.current_menu = main_menu
        
    def _is_symbol_in_menus(self, symbol: KconfigSymbol) -> bool:
        """检查符号是否已在某个菜单中"""
        for menu in self.parser.menus:
            if symbol in menu.items:
                return True
        return False
        
    def show_menu(self, menu: KconfigMenu) -> bool:
        """显示菜单"""
        while True:
            self._clear_screen()
            print(f"=== {menu.title} ===\n")
            
            # 显示菜单项
            visible_items = []
            for i, item in enumerate(menu.items):
                if self._is_item_visible(item):
                    visible_items.append((i, item))
                    
            if not visible_items:
                print("没有可用的选项")
                input("按 Enter 返回...")
                return False
                
            for idx, (original_idx, item) in enumerate(visible_items, 1):
                self._display_item(idx, item)
                
            print()
            if self.menu_stack:
                print("b. 返回上级菜单")
            print("s. 保存配置")
            print("q. 退出")
            
            choice = input("\n请选择: ").strip().lower()
            
            if choice == 's':
                return True
            elif choice == 'q':
                return False
            elif choice == 'b' and self.menu_stack:
                return False
            elif choice.isdigit():
                idx = int(choice) - 1
                if 0 <= idx < len(visible_items):
                    original_idx, item = visible_items[idx]
                    if not self._handle_item_selection(item):
                        return False
            else:
                print("无效选择，请重试。")
                input("按 Enter 继续...")
                
    def _clear_screen(self):
        """清屏"""
        os.system('clear' if os.name == 'posix' else 'cls')
        
    def _is_item_visible(self, item) -> bool:
        """检查项目是否可见"""
        if isinstance(item, KconfigSymbol):
            return item.visible and item.prompt
        elif isinstance(item, KconfigMenu):
            # 检查菜单是否有可见的子项
            return any(self._is_item_visible(subitem) for subitem in item.items)
        elif isinstance(item, KconfigChoice):
            return True
        return True
        
    def _display_item(self, idx: int, item):
        """显示菜单项"""
        if isinstance(item, KconfigSymbol):
            self._display_symbol(idx, item)
        elif isinstance(item, KconfigMenu):
            print(f"{idx}. {item.title} --->")
        elif isinstance(item, KconfigChoice):
            self._display_choice(idx, item)
            
    def _display_symbol(self, idx: int, symbol: KconfigSymbol):
        """显示符号"""
        value = self.config.get(symbol.name, 'n')
        
        if symbol.type == 'bool':
            marker = '[*]' if value == 'y' else '[ ]'
            print(f"{idx}. {marker} {symbol.prompt}")
        elif symbol.type == 'tristate':
            if value == 'y':
                marker = '[*]'
            elif value == 'm':
                marker = '[M]'
            else:
                marker = '[ ]'
            print(f"{idx}. {marker} {symbol.prompt}")
        elif symbol.type in ['string', 'int', 'hex']:
            display_value = value if value != 'n' else ''
            print(f"{idx}. {symbol.prompt} ({display_value})")
            
    def _display_choice(self, idx: int, choice: KconfigChoice):
        """显示选择组"""
        selected = None
        for option in choice.options:
            if self.config.get(option) == 'y':
                symbol = self.parser.symbols.get(option)
                if symbol and symbol.prompt:
                    selected = symbol.prompt
                    break
                    
        prompt = choice.prompt or "Choice"
        selected_text = f" ({selected})" if selected else " (none)"
        print(f"{idx}. {prompt}{selected_text} --->")
        
    def _handle_item_selection(self, item) -> bool:
        """处理项目选择"""
        if isinstance(item, KconfigSymbol):
            return self._handle_symbol_selection(item)
        elif isinstance(item, KconfigMenu):
            return self._handle_menu_selection(item)
        elif isinstance(item, KconfigChoice):
            return self._handle_choice_selection(item)
        return True
        
    def _handle_symbol_selection(self, symbol: KconfigSymbol) -> bool:
        """处理符号选择"""
        if symbol.type == 'bool':
            current = self.config.get(symbol.name, 'n')
            self.config[symbol.name] = 'n' if current == 'y' else 'y'
            self._update_dependencies()
        elif symbol.type == 'tristate':
            current = self.config.get(symbol.name, 'n')
            if current == 'n':
                self.config[symbol.name] = 'y'
            elif current == 'y':
                self.config[symbol.name] = 'm'
            else:
                self.config[symbol.name] = 'n'
            self._update_dependencies()
        elif symbol.type in ['string', 'int', 'hex']:
            self._edit_symbol_value(symbol)
            
        return True
        
    def _handle_menu_selection(self, menu: KconfigMenu) -> bool:
        """处理菜单选择"""
        self.menu_stack.append(self.current_menu)
        result = self.show_menu(menu)
        self.menu_stack.pop()
        return result
        
    def _handle_choice_selection(self, choice: KconfigChoice) -> bool:
        """处理选择组选择"""
        self._clear_screen()
        print(f"=== {choice.prompt or 'Choice'} ===\n")
        
        # 显示选项
        for i, option in enumerate(choice.options, 1):
            symbol = self.parser.symbols.get(option)
            if symbol and symbol.prompt:
                current = self.config.get(option, 'n')
                marker = '(*) ' if current == 'y' else '( ) '
                print(f"{i}. {marker}{symbol.prompt}")
                
        print("\nb. 返回")
        
        while True:
            choice_input = input("请选择: ").strip().lower()
            
            if choice_input == 'b':
                return True
            elif choice_input.isdigit():
                idx = int(choice_input) - 1
                if 0 <= idx < len(choice.options):
                    # 清除所有选项
                    for option in choice.options:
                        self.config[option] = 'n'
                    # 设置选中的选项
                    self.config[choice.options[idx]] = 'y'
                    self._update_dependencies()
                    return True
            
            print("无效选择，请重试。")
            
    def _edit_symbol_value(self, symbol: KconfigSymbol):
        """编辑符号值"""
        current = self.config.get(symbol.name, '')
        if current == 'n':
            current = ''
            
        print(f"\n编辑 {symbol.prompt}:")
        if symbol.help_text:
            print(f"说明: {symbol.help_text}")
            
        if symbol.type == 'int':
            if symbol.range_min and symbol.range_max:
                print(f"范围: {symbol.range_min} - {symbol.range_max}")
        elif symbol.type == 'hex':
            print("请输入十六进制值 (不带 0x 前缀)")
            
        new_value = input(f"当前值: '{current}', 新值: ").strip()
        
        if new_value == '':
            self.config[symbol.name] = 'n'
        else:
            # 简单验证
            if symbol.type == 'int':
                try:
                    int_val = int(new_value)
                    if symbol.range_min and symbol.range_max:
                        min_val = int(symbol.range_min)
                        max_val = int(symbol.range_max)
                        if not (min_val <= int_val <= max_val):
                            print(f"值超出范围 {min_val}-{max_val}")
                            input("按 Enter 继续...")
                            return
                    self.config[symbol.name] = new_value
                except ValueError:
                    print("无效的整数值")
                    input("按 Enter 继续...")
                    return
            elif symbol.type == 'hex':
                try:
                    int(new_value, 16)
                    self.config[symbol.name] = new_value
                except ValueError:
                    print("无效的十六进制值")
                    input("按 Enter 继续...")
                    return
            else:
                self.config[symbol.name] = new_value
                
    def _update_dependencies(self):
        """更新依赖关系"""
        # 更新可见性
        self.parser.update_visibility(self.config)
        
        # 应用 select 语句
        self.parser.apply_selects(self.config)
        
        # 解决选择组
        self.parser.resolve_choices(self.config)


def main():
    if len(sys.argv) < 3:
        print("Usage: menuconfig_improved.py <root_dir> <config_file>")
        sys.exit(1)
        
    root_dir = sys.argv[1]
    config_file = sys.argv[2]
    
    # 创建解析器
    parser = KconfigParser(root_dir)
    parser.parse_file('Kconfig')
    
    # 加载配置
    config = parser.load_config(config_file)
    parser.apply_defaults(config)
    parser.update_visibility(config)
    
    # 运行交互界面
    ui = MenuConfigUI(parser, config)
    
    save = ui.run()
    
    if save:
        # 最终处理
        parser.apply_selects(config)
        parser.resolve_choices(config)
        
        # 验证配置
        errors = parser.validate_config(config)
        if errors:
            print("\n配置验证错误:")
            for error in errors:
                print(f"  - {error}")
            print("配置可能不一致，但仍将保存。")
        
        # 保存配置
        parser.save_config(config, config_file)
        
        # 生成 autoconf.h
        output_dir = os.path.dirname(config_file)
        parser.generate_autoconf(config, os.path.join(output_dir, 'include', 'autoconf.h'))
        
        print("配置已保存")
    else:
        print("配置未保存")


if __name__ == '__main__':
    main()