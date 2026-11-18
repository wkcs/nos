#!/usr/bin/env python3
"""Exercise the actual configuration worker, HID engine and transactional store."""
from pathlib import Path
import os, subprocess, tempfile, sys
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="kb-service-") as tmp:
    tmp=Path(tmp)
    script=tmp/'registry.cmake'
    script.write_text(f'set(CONFIG_KEYBOARD_LAYOUT_ANSI61 ON)\ninclude("{root}/app/server/keyboard/definitions/sources.cmake")\nfile(WRITE "{tmp}/sources.txt" "${{APP_SRCS}}")\n')
    subprocess.run(['cmake','-P',str(script)],cwd=tmp,check=True)
    sources=(tmp/'sources.txt').read_text().split(';')
    sources += [str(root/'app/server/keyboard'/f) for f in ('keyboard_document.c','keyboard_object_store.c','keyboard_config_service.c','keyboard_config_model.c','keyboard_config_rpc.c')]
    sources += [str(root/'app/server/keyboard/keyboard_engine.c')]
    exe=tmp/'test'
    gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',gc,
        '-I'+str(root/'tests/host/keyboard_store_stubs'),'-I'+str(root/'tests/host/stubs'),'-I'+str(root/'include'),
        str(root/'tests/host/test_keyboard_config_service.c'),*sources,'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
