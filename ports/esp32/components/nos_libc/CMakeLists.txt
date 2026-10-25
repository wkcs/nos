# Copyright (C) 2026 NOS contributors.
set(NOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(PORT "${NOS_ROOT}/libc/port/esp32")
idf_component_register(SRCS "${PORT}/io.c" "${PORT}/streams.c"
    "${PORT}/directories.c" "${PORT}/select.c"
    "${NOS_ROOT}/user/lib/posix_poll.c"
    INCLUDE_DIRS "${PORT}/include" "${NOS_ROOT}/include"
    REQUIRES freertos nos_vfs vfs esp_system esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-iquote${CMAKE_CURRENT_LIST_DIR}/../../../../include/nos")
set(wrapped _open_r _close_r _read_r _write_r _lseek_r _fcntl_r _fstat_r _stat_r
    _unlink_r _rename_r _link_r _isatty_r _fopen_r _fdopen_r _fclose_r _freopen_r
    fopen fdopen fclose freopen fcntl
    mkdir rmdir chdir getcwd dup dup2 pipe fsync ftruncate truncate pread pwrite
    opendir readdir readdir_r closedir telldir seekdir rewinddir select poll)
foreach(symbol IN LISTS wrapped)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${symbol}")
endforeach()
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u nos_esp_libc_activate")
