# Copyright (C) 2026 NOS contributors.
set(NOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
idf_component_register(SRCS "backends.c" "block.c"
    "${NOS_ROOT}/servers/vfs/server.c" "${NOS_ROOT}/user/lib/string.c"
    "${NOS_ROOT}/user/lib/ipc.c" "${NOS_ROOT}/user/lib/fs_io.c"
    "${NOS_ROOT}/user/lib/fs_path.c"
    INCLUDE_DIRS "${NOS_ROOT}/include" REQUIRES freertos nos_devices heap)
target_compile_options(${COMPONENT_LIB} PRIVATE "-iquote${NOS_ROOT}/include/nos")
# Keep NOS string helpers, but use the SDK Newlib's public string symbols.
set_source_files_properties("${NOS_ROOT}/user/lib/string.c"
    PROPERTIES COMPILE_DEFINITIONS "NOS_NEWLIB=1")
