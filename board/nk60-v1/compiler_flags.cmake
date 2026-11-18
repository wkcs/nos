# The NK60 firmware shares a 240 KiB internal flash region with its shell,
# filesystem and protocol services. Prefer code size without dropping them.
set(NOS_OPTIMIZATION_FLAG -Os)
# Keep the application services inside the V1 slot at the board level.
set(NOS_APP_OPTIMIZATION_FLAG -Oz)
if(NOT CONFIG_KALLSYMS)
    set(NOS_APP_COMPILE_OPTIONS -fomit-frame-pointer)
endif()
