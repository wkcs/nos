cmake_minimum_required(VERSION 3.20)

# Default to arm-none-eabi toolchain when no toolchain file was explicitly specified
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains/arm-none-eabi.cmake"
        CACHE FILEPATH "NOS Cross-Compilation Toolchain File")
endif()

project(nos VERSION 1.0.0 LANGUAGES C ASM)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/kconfig.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/compiler_flags.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/helpers.cmake")

# ------------------------------------------------------------------------------
# Global Header Search Contract (nos_headers INTERFACE library)
# ------------------------------------------------------------------------------
add_library(nos_headers INTERFACE)

set(_NOS_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/user/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/arch/${CONFIG_ARCH}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/lua/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/lvgl/lib"
)

if(CONFIG_SOC_STM32F1)
    list(APPEND _NOS_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/soc/stm32f1/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/soc/stm32f1/inside-include"
        "${CMAKE_CURRENT_SOURCE_DIR}/soc/stm32f1/drivers/inc"
    )
endif()

if(CONFIG_SOC_STM32F4)
    list(APPEND _NOS_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/soc/stm32f4/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/soc/stm32f4/inside-include"
        "${CMAKE_CURRENT_SOURCE_DIR}/soc/stm32f4/drivers/inc"
    )
endif()

if(DEFINED CONFIG_BOARD AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}/include")
        list(APPEND _NOS_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}/include")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}/inside-include")
        list(APPEND _NOS_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}/inside-include")
    endif()
endif()

target_include_directories(nos_headers INTERFACE ${_NOS_INCLUDE_DIRS})

# ------------------------------------------------------------------------------
# Subsystems
# ------------------------------------------------------------------------------
add_subdirectory(init)
add_subdirectory(arch)
add_subdirectory(soc)
add_subdirectory(board)
add_subdirectory(kernel)
add_subdirectory(libs)
add_subdirectory(libc)
add_subdirectory(user)
add_subdirectory(drivers)
add_subdirectory(fs)
add_subdirectory(app)

set(NOS_OBJECT_LIBS
    $<TARGET_OBJECTS:nos_init>
    $<TARGET_OBJECTS:nos_arch>
    $<TARGET_OBJECTS:nos_kernel>
    $<TARGET_OBJECTS:nos_libs>
    $<TARGET_OBJECTS:nos_libc>
    $<TARGET_OBJECTS:nos_user>
    $<TARGET_OBJECTS:nos_drivers>
    $<TARGET_OBJECTS:nos_fs>
)

get_target_property(_SOC_TYPE nos_soc TYPE)
if("${_SOC_TYPE}" STREQUAL "OBJECT_LIBRARY")
    list(APPEND NOS_OBJECT_LIBS $<TARGET_OBJECTS:nos_soc>)
endif()

get_target_property(_BOARD_TYPE nos_board TYPE)
if("${_BOARD_TYPE}" STREQUAL "OBJECT_LIBRARY")
    list(APPEND NOS_OBJECT_LIBS $<TARGET_OBJECTS:nos_board>)
endif()

get_target_property(_APP_TYPE nos_app TYPE)
if("${_APP_TYPE}" STREQUAL "OBJECT_LIBRARY")
    list(APPEND NOS_OBJECT_LIBS $<TARGET_OBJECTS:nos_app>)
endif()

# ------------------------------------------------------------------------------
# Linker Script Preprocessing & Two-Pass Kallsyms Firmware Link
# ------------------------------------------------------------------------------
set(BOARD_LD_S "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}/nos_kernel.ld.S")
set(BOARD_LD_OUT "${CMAKE_CURRENT_BINARY_DIR}/board/${CONFIG_BOARD}/nos_kernel.ld")

if(EXISTS "${BOARD_LD_S}")
    set(_LD_INC_FLAGS "")
    foreach(_inc IN LISTS _NOS_INCLUDE_DIRS)
        list(APPEND _LD_INC_FLAGS "-I${_inc}")
    endforeach()
    set(_LD_DEF_FLAGS "")
    foreach(_def IN LISTS NOS_GLOBAL_DEFINES)
        list(APPEND _LD_DEF_FLAGS "-D${_def}")
    endforeach()

    add_custom_command(
        OUTPUT "${BOARD_LD_OUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/board/${CONFIG_BOARD}"
        COMMAND "${CMAKE_C_COMPILER}" -E -P -x c ${_LD_INC_FLAGS} ${_LD_DEF_FLAGS} -o "${BOARD_LD_OUT}" "${BOARD_LD_S}"
        DEPENDS "${BOARD_LD_S}" "${CMAKE_CURRENT_BINARY_DIR}/include/autocfg.h"
        COMMENT "Preprocessing linker script ${CONFIG_BOARD}/nos_kernel.ld.S"
        VERBATIM
    )
    add_custom_target(nos_linker_script DEPENDS "${BOARD_LD_OUT}")

    # Pass 1: empty kallsyms table
    set(KALLSYMS_EMPTY_C "${CMAKE_CURRENT_BINARY_DIR}/kallsyms_empty.c")
    add_custom_command(
        OUTPUT "${KALLSYMS_EMPTY_C}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkkallsyms.py"
                --empty "${KALLSYMS_EMPTY_C}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkkallsyms.py"
        COMMENT "Generating empty kallsyms table (pass 1)"
        VERBATIM
    )

    add_executable(nos_pass1.elf ${NOS_OBJECT_LIBS} "${KALLSYMS_EMPTY_C}")
    add_dependencies(nos_pass1.elf nos_linker_script)
    nos_configure_kernel_target(nos_pass1.elf)
    target_link_options(nos_pass1.elf PRIVATE
        ${NOS_LINK_FLAGS}
        "-T${BOARD_LD_OUT}"
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/nos_pass1.map"
    )
    target_link_libraries(nos_pass1.elf PRIVATE -lgcc -lc -lm)

    # Pass 2: real kallsyms table extracted via Python + objdump (Windows/macOS/Linux safe)
    set(KALLSYMS_REAL_C "${CMAKE_CURRENT_BINARY_DIR}/kallsyms_table.c")
    add_custom_command(
        OUTPUT "${KALLSYMS_REAL_C}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkkallsyms.py"
                --objdump "${CMAKE_OBJDUMP}"
                --elf "$<TARGET_FILE:nos_pass1.elf>"
                --out "${KALLSYMS_REAL_C}"
        DEPENDS nos_pass1.elf "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkkallsyms.py"
        COMMENT "Extracting kernel symbol table into kallsyms_table.c"
        VERBATIM
    )

    add_executable(nos.elf ${NOS_OBJECT_LIBS} "${KALLSYMS_REAL_C}")
    add_dependencies(nos.elf nos_linker_script)
    nos_configure_kernel_target(nos.elf)
    target_link_options(nos.elf PRIVATE
        ${NOS_LINK_FLAGS}
        "-T${BOARD_LD_OUT}"
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/nos.map"
    )
    target_link_libraries(nos.elf PRIVATE -lgcc -lc -lm)

    # Firmware artifacts: nos.bin, nos.hex, nos.img, nos.list, and size report
    set(NOS_BIN  "${CMAKE_CURRENT_BINARY_DIR}/nos.bin")
    set(NOS_HEX  "${CMAKE_CURRENT_BINARY_DIR}/nos.hex")
    set(NOS_IMG  "${CMAKE_CURRENT_BINARY_DIR}/nos.img")
    set(NOS_LIST "${CMAKE_CURRENT_BINARY_DIR}/nos.list")

    add_custom_command(
        OUTPUT "${NOS_BIN}" "${NOS_HEX}" "${NOS_IMG}" "${NOS_LIST}"
        COMMAND "${CMAKE_OBJCOPY}" "$<TARGET_FILE:nos.elf>" "${NOS_BIN}" -Obinary
        COMMAND "${CMAKE_OBJCOPY}" "$<TARGET_FILE:nos.elf>" "${NOS_HEX}" -Oihex
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_img.py"
                "${NOS_BIN}" "${PROJECT_VERSION_MAJOR}" "${PROJECT_VERSION_MINOR}" "${PROJECT_VERSION_PATCH}" 1 "${NOS_IMG}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkkallsyms.py"
                --objdump "${CMAKE_OBJDUMP}"
                --elf "$<TARGET_FILE:nos.elf>"
                --disasm "${NOS_LIST}"
        COMMAND "${CMAKE_SIZE}" --format=berkeley "$<TARGET_FILE:nos.elf>"
        DEPENDS nos.elf "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_img.py"
        COMMENT "Generating nos.bin, nos.hex, nos.img, nos.list and reporting firmware size"
        VERBATIM
    )

    add_custom_target(nos_firmware ALL DEPENDS "${NOS_BIN}" "${NOS_HEX}" "${NOS_IMG}" "${NOS_LIST}")
endif()

# ------------------------------------------------------------------------------
# Cross-Platform Utility Targets (Flash, Debug, QEMU, Menuconfig)
# ------------------------------------------------------------------------------
set(BOARD_OPENOCD_CFG "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}/openocd.cfg")

add_custom_target(flash
    COMMAND openocd -f "${BOARD_OPENOCD_CFG}" -c "program ${CMAKE_CURRENT_BINARY_DIR}/nos.hex verify reset exit"
    DEPENDS nos_firmware
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Flashing nos.hex via OpenOCD"
    VERBATIM
)

add_custom_target(stflash
    COMMAND st-flash --connect-under-reset write "${CMAKE_CURRENT_BINARY_DIR}/nos.bin" 0x08000000
    DEPENDS nos_firmware
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Flashing nos.bin via st-flash"
    VERBATIM
)

if("${CONFIG_BOARD}" STREQUAL "qemu-stm32")
    add_custom_target(qemu-run
        COMMAND qemu-system-arm -M netduinoplus2 -nographic -semihosting -semihosting-config enable=on,target=native -kernel "$<TARGET_FILE:nos.elf>"
        DEPENDS nos.elf
        COMMENT "Running nos.elf in QEMU (netduinoplus2)"
        VERBATIM
    )
    add_custom_target(qemu-run-gdb
        COMMAND qemu-system-arm -M netduinoplus2 -nographic -semihosting -semihosting-config enable=on,target=native -kernel "$<TARGET_FILE:nos.elf>" -S -s
        DEPENDS nos.elf
        COMMENT "Running nos.elf in QEMU waiting for GDB on :1234"
        VERBATIM
    )
endif()

add_custom_target(menuconfig
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/menuconfig.py" Kconfig
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Launching interactive Kconfig menuconfig"
    VERBATIM
)

add_custom_target(savedefconfig
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/savedefconfig.py"
            "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/.config" "${NOS_DEFCONFIG}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Saving minimal defconfig to ${NOS_DEFCONFIG}"
    VERBATIM
)
