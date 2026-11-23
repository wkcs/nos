# NuTiny-NUC472 采用 384 KiB 内核分区 + 128 KiB LittleFS 分区，启用 -Os 保证充足 Flash 余量
set(NOS_OPTIMIZATION_FLAG -Os)
# 应用层（nShell/Lua/NLK）启用 -Oz，与 NK60 保持一致，避免挤占 128 KiB LittleFS 分区
set(NOS_APP_OPTIMIZATION_FLAG -Oz)
# 第三方 Lua 虚拟机启用 -fomit-frame-pointer 释放约 4 KiB Flash，保留内核/驱动/Shell 的帧指针回溯
set(NOS_LUA_COMPILE_OPTIONS -fomit-frame-pointer)
