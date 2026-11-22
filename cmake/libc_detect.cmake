# 探测当前工具链是否自带标准 C 库头文件（例如 Newlib / Picolibc / musl）
set(_NOS_LIBC_TEST_C "${CMAKE_BINARY_DIR}/CMakeFiles/nos_libc_detect.c")
set(_NOS_LIBC_TEST_I "${CMAKE_BINARY_DIR}/CMakeFiles/nos_libc_detect.i")
file(WRITE "${_NOS_LIBC_TEST_C}" "#include <string.h>\nint nos_libc_probe(void) { return 0; }\n")

execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -E "${_NOS_LIBC_TEST_C}" -o "${_NOS_LIBC_TEST_I}"
    RESULT_VARIABLE _NOS_LIBC_PROBE_RC
    OUTPUT_QUIET
    ERROR_QUIET
)

if(_NOS_LIBC_PROBE_RC EQUAL 0)
    set(NOS_HAS_TOOLCHAIN_LIBC TRUE)
else()
    set(NOS_HAS_TOOLCHAIN_LIBC FALSE)
    message(STATUS "[Libc] 未检测到工具链标准 C 库，启用 NOS 内核隔离式 freestanding libc")
endif()
