#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 data/ 目录下的静态文件打包成 web_assets.h 头文件
嵌入固件，OTA 升级时一并更新，无需 uploadfs

生成:
  src/web_assets.h - 包含 index_html/style_css/app_js/config_json
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "data")
OUTPUT = os.path.join(ROOT, "src", "web_assets.h")

FILES = [
    ("index.html", "index_html", "text/html"),
    ("style.css", "style_css", "text/css"),
    ("app.js", "app_js", "application/javascript"),
    ("config.json", "config_json", "application/json"),
]

def encode(data: bytes) -> str:
    """编码为 C 字符串字面量（每行 80 字符以内）"""
    lines = []
    line = '"'
    for b in data:
        c = chr(b)
        # 转义特殊字符
        if c == '\\':
            line += '\\\\'
        elif c == '"':
            line += '\\"'
        elif c == '\n':
            line += '\\n'
        elif c == '\r':
            line += '\\r'
        elif c == '\t':
            line += '\\t'
        elif 32 <= b < 127:
            line += c
        else:
            line += '\\x%02x' % b
        # 控制每行长度
        if len(line) >= 78:
            line += '"'
            lines.append(line)
            line = '"'
    if line != '"':
        line += '"'
        lines.append(line)
    return '\n'.join(lines)


def main():
    out = []
    out.append('// 自动生成 - 不要手动修改')
    out.append('// 由 tools/gen_web_assets.py 从 data/ 目录生成')
    out.append('// OTA 升级固件时一并更新，无需 uploadfs')
    out.append('#pragma once'
               '')
    out.append('#include <Arduino.h>')
    out.append('')
    out.append('namespace WebAssets {')
    out.append('')

    for fname, var, mime in FILES:
        path = os.path.join(DATA_DIR, fname)
        if not os.path.exists(path):
            print(f'[warn] 文件不存在: {path}', file=sys.stderr)
            continue
        with open(path, 'rb') as f:
            data = f.read()
        print(f'  {fname}: {len(data)} bytes')
        out.append(f'// {fname} ({len(data)} bytes, {mime})')
        out.append(f'const char {var}[] PROGMEM = R"=====(')
        # 用 raw string literal 避免转义问题
        # 但需要确保数据中不包含 )=====( 这个分隔符
        text = data.decode('utf-8', errors='replace')
        if ')=====(' in text:
            print(f'[error] {fname} 包含分隔符 )=====(', file=sys.stderr)
            sys.exit(1)
        out.append(text)
        out.append(')=====";')
        out.append(f'const size_t {var}_len = {len(data)};')
        out.append('')

    out.append('}  // namespace WebAssets')
    out.append('')

    content = '\n'.join(out)
    with open(OUTPUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    print(f'\n生成: {OUTPUT} ({len(content)} bytes)')


if __name__ == '__main__':
    main()
