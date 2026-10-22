# Third-party source

<!-- Copyright (C) 2026 NOS contributors -->

The Lua and LVGL sources were moved from the old RTOS application tree. NOS
builds them as libraries for independently linked user applications with
`scripts/build_user_libraries.py`. Platform configuration and application code
belong in `apps/lua/` and `apps/lvgl/`, rather than in these vendor directories.

| Directory | Bundled version | License |
| --- | --- | --- |
| `lua/` | Lua 5.4.6 | MIT; full notice at the end of `lua/lua.h` |
| `lvgl/` | LVGL 9.1.0 | MIT; full notice in [`lvgl/LICENCE.txt`](lvgl/LICENCE.txt), restored from [upstream v9.1.0](https://github.com/lvgl/lvgl/blob/v9.1.0/LICENCE.txt); embedded components retain their own notices |

The vendor trees retain the repository's existing source and copyright
notices. The obsolete RTOS Makefiles and LVGL adapters that directly accessed
the old privileged kernel have been removed. The active LVGL port selects
`LV_OS_NONE` and C library allocation, strings and formatting in
`apps/lvgl/lv_conf.h`.
