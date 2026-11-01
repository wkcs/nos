# The NK60 firmware shares a 240 KiB internal flash region with its shell,
# filesystem and protocol services. Prefer code size without dropping them.
set(NOS_OPTIMIZATION_FLAG -Os)
