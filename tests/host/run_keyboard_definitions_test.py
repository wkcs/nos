#!/usr/bin/env python3
"""Build the same generated definition registry used by firmware."""
from pathlib import Path
import os
import subprocess
import tempfile
import sys
import shutil

root = Path(__file__).resolve().parents[2]

def compile_definitions(test, extra_sources=(), extra_flags=(), extensions=False):
    with tempfile.TemporaryDirectory(prefix="keyboard-definitions-") as tmp:
        tmp = Path(tmp)
        definitions = root / "app/server/keyboard/definitions"
        if extensions:
            copied = tmp / "definitions"
            shutil.copytree(definitions, copied)
            definitions = copied
            (definitions / "actions/probe.c").write_text('''/** Copyright (C) 2026 Nick Hu. */
#include <nos/keyboard_definitions.h>
static int execute(const struct key_action_context *context, const void *arguments,
                   uint16_t length, const struct kb_action_runtime *runtime)
{
    (void)context; (void)arguments; (void)length; (void)runtime;
    return 42;
}
const struct kb_action_definition kb_action_probe = {
    .id = "probe", .description = "Registry extension", .execute = execute,
};
''')
            (definitions / "effects/probe.c").write_text('''/** Copyright (C) 2026 Nick Hu. */
#include <nos/keyboard_definitions.h>
static void render(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y,
                   uint32_t phase, uint8_t rgb[3], void *priv)
{
    (void)cfg; (void)x; (void)y; (void)phase; (void)priv;
    rgb[0] = 42; rgb[1] = 0; rgb[2] = 0;
}
const struct kb_effect_definition kb_effect_probe = {
    .id = "probe", .description = "Registry extension", .engine_id = 9, .render = render,
};
''')
            extra_flags = (*extra_flags, "-DTEST_DEFINITION_EXTENSION")
        script = tmp / "registry.cmake"
        script.write_text(f'set(CONFIG_KEYBOARD_LAYOUT_ANSI61 ON)\n'
                          f'include("{definitions}/sources.cmake")\n'
                          f'file(WRITE "{tmp}/sources.txt" "${{APP_SRCS}}")\n')
        subprocess.run(["cmake", "-P", str(script)], cwd=tmp, check=True)
        sources = (tmp / "sources.txt").read_text().split(";")
        sources += [str(root / "app/server/keyboard/keyboard_document.c"),
                    str(root / "app/server/keyboard/keyboard_engine.c")]
        gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        exe = tmp / "test"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-ffunction-sections", "-fdata-sections", gc,
                        "-DCONFIG_KEYBOARD_LIGHT_MAX_LAYERS=8", *extra_flags, "-I", str(root / "tests/host/stubs"),
                        "-I", str(root / "include"), str(root / "tests/host" / test),
                        *sources, *map(str, extra_sources), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    compile_definitions("test_keyboard_definitions.c")
    compile_definitions("test_keyboard_definition_registry.c", extensions=True)
    compile_definitions("test_keyboard_definition_registry.c")
