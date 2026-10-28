#!/usr/bin/env python3
"""Check that portable kernel layers do not depend on board headers."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
RULES = (
    ('kernel', re.compile(r'^\s*#\s*include\s*[<"](?:board/board\.h|stm32f1|stm32f4)', re.M)),
    ('arch/arm/cpu', re.compile(r'^\s*#\s*include\s*[<"]board/board\.h[>"]', re.M)),
    ('drivers/serial', re.compile(r'^\s*#\s*include\s*[<"]board/board\.h[>"]', re.M)),
    ('drivers/block', re.compile(r'^\s*#\s*include\s*[<"]board/board\.h[>"]', re.M)),
    ('fs', re.compile(r'^\s*#\s*include\s*[<"]board/board\.h[>"]', re.M)),
)


def main():
    failed = False
    for relative, pattern in RULES:
        for path in (ROOT / relative).rglob('*'):
            if path.suffix not in ('.c', '.h') or not path.is_file():
                continue
            source = path.read_text(encoding='utf-8')
            for match in pattern.finditer(source):
                line = source.count('\n', 0, match.start()) + 1
                print(f'{path.relative_to(ROOT)}:{line}: forbidden layer dependency', file=sys.stderr)
                failed = True
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
