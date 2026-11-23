set(USER_LIBNOS_SRCS
    libnos/ipc_client.c
    libnos/posix_fd.c
)

if(CONFIG_ARCH_ARM64 AND CONFIG_NOS_AI_CORE)
    set(_EL0_DEMO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/apps/ai_agent_demo")
    set(_EL0_DEMO_ELF "${CMAKE_BINARY_DIR}/generated/ai_agent.elf")
    set(_EL0_DEMO_BIN_C "${CMAKE_BINARY_DIR}/generated/builtin_user_apps.c")

    set(_EL0_TOOL_SRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/apps/tool_server_demo")
    set(_EL0_TOOL_SRV_ELF "${CMAKE_BINARY_DIR}/generated/tool_srv.elf")
    set(_EL0_TOOL_SRV_BIN_C "${CMAKE_BINARY_DIR}/generated/builtin_tool_srv_app.c")

    add_custom_command(
        OUTPUT "${_EL0_DEMO_ELF}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND "${CMAKE_C_COMPILER}"
                -march=armv8-a -mbranch-protection=standard -ffreestanding -nostdlib -fno-builtin
                -fno-pie -no-pie -O2 -Wall -Wextra -Werror "-Wl,--build-id=none"
                -I "${CMAKE_SOURCE_DIR}/include"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/include"
                -T "${_EL0_DEMO_DIR}/user.ld"
                "${_EL0_DEMO_DIR}/crt0.S"
                "${_EL0_DEMO_DIR}/main.c"
                -lgcc
                -o "${_EL0_DEMO_ELF}"
        DEPENDS
            "${_EL0_DEMO_DIR}/user.ld"
            "${_EL0_DEMO_DIR}/crt0.S"
            "${_EL0_DEMO_DIR}/main.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/nos/user_ai.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/nos/ulibc.h"
            "${CMAKE_SOURCE_DIR}/include/nos/elf64.h"
            "${CMAKE_SOURCE_DIR}/include/uapi/nos/ai.h"
            "${CMAKE_SOURCE_DIR}/include/uapi/nos/syscall.h"
        COMMENT "Building standalone AArch64 EL0 C app ai_agent.elf"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${_EL0_DEMO_BIN_C}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/bin2c.py"
                --symbol g_builtin_ai_agent_elf
                "${_EL0_DEMO_ELF}"
                "${_EL0_DEMO_BIN_C}"
        DEPENDS "${_EL0_DEMO_ELF}" "${CMAKE_SOURCE_DIR}/scripts/bin2c.py"
        COMMENT "Embedding ai_agent.elf into builtin_user_apps.c"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${_EL0_TOOL_SRV_ELF}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND "${CMAKE_C_COMPILER}"
                -march=armv8-a -mbranch-protection=standard -ffreestanding -nostdlib -fno-builtin
                -fPIE -pie -O2 -Wall -Wextra -Werror "-Wl,--build-id=none"
                -I "${CMAKE_SOURCE_DIR}/include"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/include"
                -T "${_EL0_TOOL_SRV_DIR}/user.ld"
                "${_EL0_TOOL_SRV_DIR}/crt0.S"
                "${_EL0_TOOL_SRV_DIR}/main.c"
                -lgcc
                -o "${_EL0_TOOL_SRV_ELF}"
        DEPENDS
            "${_EL0_TOOL_SRV_DIR}/user.ld"
            "${_EL0_TOOL_SRV_DIR}/crt0.S"
            "${_EL0_TOOL_SRV_DIR}/main.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/nos/user_ai.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/nos/ulibc.h"
            "${CMAKE_SOURCE_DIR}/include/nos/elf64.h"
            "${CMAKE_SOURCE_DIR}/include/uapi/nos/ai.h"
            "${CMAKE_SOURCE_DIR}/include/uapi/nos/syscall.h"
        COMMENT "Building standalone AArch64 EL0 PIE C app tool_srv.elf"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${_EL0_TOOL_SRV_BIN_C}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/bin2c.py"
                --symbol g_builtin_tool_srv_elf
                "${_EL0_TOOL_SRV_ELF}"
                "${_EL0_TOOL_SRV_BIN_C}"
        DEPENDS "${_EL0_TOOL_SRV_ELF}" "${CMAKE_SOURCE_DIR}/scripts/bin2c.py"
        COMMENT "Embedding tool_srv.elf into builtin_tool_srv_app.c"
        VERBATIM
    )

    list(APPEND USER_LIBNOS_SRCS "${_EL0_DEMO_BIN_C}" "${_EL0_TOOL_SRV_BIN_C}")
    if(CONFIG_VFS AND CONFIG_RAMFS)
        list(APPEND USER_LIBNOS_SRCS libnos/builtin_apps_fs.c)
    endif()
endif()

add_library(nos_user OBJECT ${USER_LIBNOS_SRCS})
nos_configure_kernel_target(nos_user)
