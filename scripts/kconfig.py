#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
简化的 Kconfig 解析器和配置生成器
Copyright (C) 2023-2024 Nick Hu
"""

import os
import sys
import re
from typing import Dict, List, Optional, Set, Tuple


class KconfigSymbol:
    """Kconfig 符号（配置选项）"""
    
    def __init__(self, name: str, type_: str = 'bool'):
        self.name = name
        self.type = type_  # bool, tristate, string, int, hex
        self.prompt = None
        self.default = None
        self.depends = []
        self.selects = []
        self.help_text = ""
        self.range_min = None
        self.range_max = None
        self.value = None
        
    def __repr__(self):
        return f"KconfigSymbol({self.name}, type={self.type}, value={self.value})"


class KconfigChoice:
    """Kconfig 选择组"""
    
    def __init__(self):
        self.prompt = None
        self.default = None
        self.options = []
        
    def __repr__(self):
        return f"KconfigChoice(prompt={self.prompt}, options={self.options})"


class KconfigParser:
    """Kconfig 解析器"""
    
    def __init__(self, root_dir: str = '.'):
        self.root_dir = root_dir
        self.symbols: Dict[str, KconfigSymbol] = {}
        self.choices: List[KconfigChoice] = []
        self.current_symbol: Optional[KconfigSymbol] = None
        self.current_choice: Optional[KconfigChoice] = None
        self.if_stack: List[str] = []
        
    def parse_file(self, filename: str):
        """解析 Kconfig 文件"""
        filepath = os.path.join(self.root_dir, filename)
        if not os.path.exists(filepath):
            return
            
        with open(filepath, 'r', encoding='utf-8') as f:
            lines = f.readlines()
            
        i = 0
        while i < len(lines):
            line = lines[i].rstrip()
            i += 1
            
            # 跳过空行和注释
            if not line or line.strip().startswith('#'):
                continue
                
            # 处理续行
            while line.endswith('\\') and i < len(lines):
                line = line[:-1] + ' ' + lines[i].strip()
                i += 1
                
            # 解析行
            self._parse_line(line)
            
    def _parse_line(self, line: str):
        """解析单行"""
        stripped = line.strip()
        indent = len(line) - len(line.lstrip())
        
        # source 指令
        if stripped.startswith('source '):
            source_file = stripped[7:].strip().strip('"')
            # 处理条件 source
            if ' if ' in source_file:
                source_file = source_file.split(' if ')[0].strip()
            self.parse_file(source_file)
            return
            
        # mainmenu
        if stripped.startswith('mainmenu '):
            return
            
        # menu
        if stripped.startswith('menu '):
            return
            
        # endmenu
        if stripped.startswith('endmenu'):
            return
            
        # if 语句
        if stripped.startswith('if '):
            condition = stripped[3:].strip()
            self.if_stack.append(condition)
            return
            
        # endif
        if stripped.startswith('endif'):
            if self.if_stack:
                self.if_stack.pop()
            return
            
        # choice
        if stripped.startswith('choice'):
            self.current_choice = KconfigChoice()
            self.choices.append(self.current_choice)
            self.current_symbol = None
            return
            
        # endchoice
        if stripped.startswith('endchoice'):
            self.current_choice = None
            return
            
        # config
        if stripped.startswith('config '):
            name = stripped[7:].strip()
            self.current_symbol = KconfigSymbol(name)
            self.symbols[name] = self.current_symbol
            if self.current_choice:
                self.current_choice.options.append(name)
            return
            
        # 属性
        if self.current_symbol or self.current_choice:
            self._parse_property(stripped)
            
    def _parse_property(self, line: str):
        """解析属性"""
        target = self.current_symbol if self.current_symbol else self.current_choice
        
        # bool, tristate, string, int, hex
        for type_ in ['bool', 'tristate', 'string', 'int', 'hex']:
            if line.startswith(type_):
                if self.current_symbol:
                    self.current_symbol.type = type_
                    rest = line[len(type_):].strip()
                    if rest and rest[0] == '"':
                        self.current_symbol.prompt = rest.strip('"')
                return
                
        # prompt
        if line.startswith('prompt '):
            prompt = line[7:].strip().strip('"')
            if target:
                target.prompt = prompt
            return
            
        # default
        if line.startswith('default '):
            default = line[8:].strip()
            if target:
                target.default = default
            return
            
        # depends on
        if line.startswith('depends on '):
            depends = line[11:].strip()
            if self.current_symbol:
                self.current_symbol.depends.append(depends)
            return
            
        # select
        if line.startswith('select '):
            select = line[7:].strip()
            if self.current_symbol:
                self.current_symbol.selects.append(select)
            return
            
        # range
        if line.startswith('range '):
            parts = line[6:].strip().split()
            if len(parts) >= 2 and self.current_symbol:
                self.current_symbol.range_min = parts[0]
                self.current_symbol.range_max = parts[1]
            return
            
        # help
        if line.startswith('help') or line.startswith('---help---'):
            # help 文本在后续行中
            return
            
    def evaluate_condition(self, condition: str, config: Dict[str, str]) -> bool:
        """评估条件表达式"""
        if not condition:
            return True
            
        # 简化的条件评估
        # 支持: SYMBOL, !SYMBOL, SYMBOL=value, SYMBOL!=value
        condition = condition.strip()
        
        # 处理 !
        if condition.startswith('!'):
            return not self.evaluate_condition(condition[1:], config)
            
        # 处理 =
        if '=' in condition:
            if '!=' in condition:
                parts = condition.split('!=')
                symbol = parts[0].strip()
                value = parts[1].strip().strip('"')
                return config.get(symbol, '') != value
            else:
                parts = condition.split('=')
                symbol = parts[0].strip()
                value = parts[1].strip().strip('"')
                return config.get(symbol, '') == value
                
        # 简单符号检查
        return config.get(condition, 'n') in ['y', 'm', 'Y', 'M']
        
    def load_config(self, config_file: str) -> Dict[str, str]:
        """加载现有配置"""
        config = {}
        if not os.path.exists(config_file):
            return config
            
        with open(config_file, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                if '=' in line:
                    key, value = line.split('=', 1)
                    key = key.strip()
                    value = value.strip()
                    
                    # 移除字符串值的引号
                    if value.startswith('"') and value.endswith('"'):
                        value = value[1:-1]
                    
                    # 移除 CONFIG_ 前缀（如果存在）
                    if key.startswith('CONFIG_'):
                        key = key[7:]
                        
                    config[key] = value
        return config
        
    def apply_defaults(self, config: Dict[str, str]):
        """应用默认值"""
        for name, symbol in self.symbols.items():
            if name not in config and symbol.default:
                default = symbol.default.strip('"')
                # 评估条件默认值
                if ' if ' in default:
                    parts = default.split(' if ')
                    value = parts[0].strip()
                    condition = parts[1].strip()
                    if self.evaluate_condition(condition, config):
                        config[name] = value
                else:
                    config[name] = default
                    
        # 处理 choice 默认值
        for choice in self.choices:
            if choice.default:
                default_option = choice.default.strip()
                if default_option in self.symbols:
                    # 确保只有一个选项被选中
                    selected = False
                    for option in choice.options:
                        if config.get(option) == 'y':
                            selected = True
                            break
                    if not selected:
                        config[default_option] = 'y'
                        
    def apply_selects(self, config: Dict[str, str]):
        """应用 select 语句"""
        changed = True
        while changed:
            changed = False
            for name, symbol in self.symbols.items():
                if config.get(name) == 'y':
                    for select in symbol.selects:
                        if config.get(select) != 'y':
                            config[select] = 'y'
                            changed = True
                            
    def save_config(self, config: Dict[str, str], output_file: str):
        """保存配置到 .config 文件"""
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write("#\n")
            f.write("# Automatically generated configuration\n")
            f.write("#\n\n")
            
            for name, symbol in sorted(self.symbols.items()):
                value = config.get(name, '')
                if not value or value == 'n':
                    f.write(f"# {name} is not set\n")
                else:
                    if symbol.type in ['string']:
                        f.write(f'{name}="{value}"\n')
                    else:
                        f.write(f'{name}={value}\n')
                        
    def generate_autoconf(self, config: Dict[str, str], output_file: str):
        """生成 autoconf.h 头文件"""
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write("/*\n")
            f.write(" * Automatically generated configuration header\n")
            f.write(" */\n")
            f.write("#ifndef __AUTOCONF_H__\n")
            f.write("#define __AUTOCONF_H__\n\n")
            
            for name, symbol in sorted(self.symbols.items()):
                value = config.get(name, '')
                if not value or value == 'n':
                    continue
                
                # 添加 CONFIG_ 前缀（如果还没有）
                config_name = name if name.startswith('CONFIG_') else f'CONFIG_{name}'
                    
                if symbol.type == 'bool' or symbol.type == 'tristate':
                    if value == 'y' or value == 'Y':
                        f.write(f"#define {config_name} 1\n")
                    elif value == 'm' or value == 'M':
                        f.write(f"#define {config_name} 2\n")
                elif symbol.type == 'string':
                    f.write(f'#define {config_name} "{value}"\n')
                elif symbol.type in ['int', 'hex']:
                    f.write(f"#define {config_name} {value}\n")
                    
            f.write("\n#endif /* __AUTOCONF_H__ */\n")


def main():
    if len(sys.argv) < 4:
        print("Usage: kconfig.py <root_dir> <defconfig> <output_dir>")
        sys.exit(1)
        
    root_dir = sys.argv[1]
    defconfig = sys.argv[2]
    output_dir = sys.argv[3]
    
    # 创建解析器
    parser = KconfigParser(root_dir)
    
    # 解析 Kconfig
    parser.parse_file('Kconfig')
    
    # 加载 defconfig
    config = parser.load_config(defconfig)
    
    # 应用默认值
    parser.apply_defaults(config)
    
    # 应用 select 语句
    parser.apply_selects(config)
    
    # 保存配置
    os.makedirs(output_dir, exist_ok=True)
    parser.save_config(config, os.path.join(output_dir, '.config'))
    
    # 生成 autoconf.h
    os.makedirs(os.path.join(output_dir, 'include'), exist_ok=True)
    parser.generate_autoconf(config, os.path.join(output_dir, 'include', 'autoconf.h'))
    
    print(f"Configuration generated successfully")
    print(f"  .config: {os.path.join(output_dir, '.config')}")
    print(f"  autoconf.h: {os.path.join(output_dir, 'include', 'autoconf.h')}")


if __name__ == '__main__':
    main()
