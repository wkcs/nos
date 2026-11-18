#!/usr/bin/env python3
"""Run identical shared renderer and factory profile checks for both NK60 boards."""
from run_keyboard_definitions_test import compile_definitions

compile_definitions("test_nk60_light_effects.c")
compile_definitions("test_nk60_light_effects.c", extra_flags=("-DTEST_NK60_V1",))
