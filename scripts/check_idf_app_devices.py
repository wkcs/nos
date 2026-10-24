#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Keep ESP-IDF peripheral headers behind the NOS platform backend."""
from pathlib import Path
import re
import sys

HARDWARE_INCLUDE = re.compile(
    r'#\s*include\s*[<"](?:driver/|hal/|soc/|usb/|esp_wifi\.h|esp_netif\.h|'
    r"nimble/|host/|nvs(?:_flash)?\.h|esp_ota_ops\.h|esp_partition\.h|"
    r"esp_lcd|esp_adc|tinyusb)"
)


def main():
    failed = False
    for filename in sys.argv[1:]:
        path = Path(filename)
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if HARDWARE_INCLUDE.search(line):
                print(
                    f"{path}:{number}: SDK peripheral header in application; use NOS devices",
                    file=sys.stderr,
                )
                failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
