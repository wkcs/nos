#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Build, audit and boot NOS on the actual official ESP32-C3 machine."""
import esp32s3 as suite

suite.port.configure_target("esp32c3")
if __name__ == "__main__":
    suite.main()
