#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""ESP32-C3 entry point using the shared, pinned ESP-IDF build environment."""
import esp32s3 as port

port.configure_target("esp32c3")
if __name__ == "__main__":
    port.main()
