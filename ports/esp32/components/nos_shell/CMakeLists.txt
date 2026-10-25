# Copyright (C) 2026 NOS contributors.
set(NOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
idf_component_register(SRCS "shell.c" "fs_commands.c" "${NOS_ROOT}/user/shell/editor.c"
    "${NOS_ROOT}/user/shell/parser.c"
    INCLUDE_DIRS "${NOS_ROOT}/include" REQUIRES nos_vfs nos_devices freertos esp_timer heap)
target_compile_options(${COMPONENT_LIB} PRIVATE "-iquote${NOS_ROOT}/include/nos")
execute_process(COMMAND "${PYTHON}" "${NOS_ROOT}/scripts/check_idf_app_devices.py"
    "${CMAKE_CURRENT_LIST_DIR}/shell.c" "${CMAKE_CURRENT_LIST_DIR}/fs_commands.c"
    COMMAND_ERROR_IS_FATAL ANY)
