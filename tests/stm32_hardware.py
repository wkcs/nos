#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Opt-in real STM32 serial acceptance runner."""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import termios
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, type=Path)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--baud', type=int, choices=(9600, 115200), default=115200)
    parser.add_argument('--flash-command', nargs='+', help='Explicit debugger command; {image} expands to --image')
    parser.add_argument('--expect', action='append', required=True, help='Required UART output; may be repeated')
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--output', type=Path, default=Path('out/microkernel/hardware/stm32'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    state = {'platform': 'stm32', 'transport': str(args.port), 'executed': False,
             'passed': False, 'expected': args.expect, 'image': str(args.image or '')}
    if not args.port.exists():
        state['reason'] = 'Physical serial device unavailable; hardware validation not executed'
    else:
        fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            attrs = termios.tcgetattr(fd)
            attrs[0] = attrs[1] = attrs[3] = 0
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[4] = attrs[5] = getattr(termios, 'B' + str(args.baud))
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            termios.tcflush(fd, termios.TCIFLUSH)
            if args.flash_command:
                if not args.image or not args.image.is_file():
                    parser.error('--flash-command requires an existing --image')
                subprocess.run([part.replace('{image}', str(args.image.resolve())) for part in args.flash_command], check=True)
            state['executed'] = True
            start = time.monotonic()
            captured = bytearray()
            while time.monotonic() - start < args.timeout and len(captured) < 4 * 1024 * 1024:
                readable, _, _ = select.select([fd], [], [], min(1, args.timeout))
                if readable:
                    captured.extend(os.read(fd, 4096))
                text = captured.decode(errors='replace')
                if all(marker in text for marker in args.expect):
                    break
            text = captured.decode(errors='replace')
            (args.output / 'serial.log').write_text(text)
            state['passed'] = all(marker in text for marker in args.expect)
            state['missing'] = [marker for marker in args.expect if marker not in text]
            state['elapsed_seconds'] = time.monotonic() - start
        finally:
            os.close(fd)
    (args.output / 'results.json').write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps(state))
    return 0 if state['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
