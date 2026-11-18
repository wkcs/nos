#!/usr/bin/env python3
"""Exercise the production engine identically with either host macro setting."""
from run_keyboard_definitions_test import compile_definitions

if __name__ == "__main__":
    for flags in ((), ("-DNLK_HOST_TEST",)):
        compile_definitions("test_keyboard_cap_runtime.c",
                            extra_flags=("-fsanitize=address,undefined", *flags))
