cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake"
        CACHE FILEPATH "ARM Bare-Metal Toolchain File")
endif()

project(nk60_bootloader VERSION 1.0.0 LANGUAGES C ASM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(BL_COMPILE_FLAGS
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -Os
    -Wall
    -Wextra
    -Werror
    -ffreestanding
    -fno-builtin
    -ffunction-sections
    -fdata-sections
    -fno-common
    -std=gnu11
)

set(BL_LD_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/nk60_bootloader.ld")

add_executable(bootloader.elf
    start.S
    main.c
)

target_include_directories(bootloader.elf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(bootloader.elf PRIVATE
    NK60_BOOTLOADER_BUILD=1
    BL_VER_MAJOR=${PROJECT_VERSION_MAJOR}
    BL_VER_MINOR=${PROJECT_VERSION_MINOR}
    BL_VER_PATCH=${PROJECT_VERSION_PATCH}
)

target_compile_options(bootloader.elf PRIVATE ${BL_COMPILE_FLAGS})

target_link_options(bootloader.elf PRIVATE
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -nostartfiles
    -nostdlib
    -Wl,--gc-sections
    -Wl,--no-warn-rwx-segments
    "-T${BL_LD_SCRIPT}"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/bootloader.map"
)
target_link_libraries(bootloader.elf PRIVATE -lgcc)

set(BL_BIN  "${CMAKE_CURRENT_BINARY_DIR}/bootloader.bin")
set(BL_HEX  "${CMAKE_CURRENT_BINARY_DIR}/bootloader.hex")
set(BL_LIST "${CMAKE_CURRENT_BINARY_DIR}/bootloader.list")

add_custom_command(
    OUTPUT "${BL_BIN}" "${BL_HEX}" "${BL_LIST}"
    COMMAND "${CMAKE_OBJCOPY}" "$<TARGET_FILE:bootloader.elf>" "${BL_BIN}" -Obinary
    COMMAND "${CMAKE_OBJCOPY}" "$<TARGET_FILE:bootloader.elf>" "${BL_HEX}" -Oihex
    COMMAND "${CMAKE_OBJDUMP}" -d -S "$<TARGET_FILE:bootloader.elf>" > "${BL_LIST}"
    COMMAND "${CMAKE_SIZE}" --format=berkeley "$<TARGET_FILE:bootloader.elf>"
    DEPENDS bootloader.elf "${BL_LD_SCRIPT}"
    COMMENT "Generating bootloader.bin, bootloader.hex and reporting size"
    VERBATIM
)

add_custom_target(bootloader_firmware ALL DEPENDS "${BL_BIN}" "${BL_HEX}" "${BL_LIST}")

set(BL_OPENOCD_CFG "${CMAKE_CURRENT_SOURCE_DIR}/openocd.cfg")
add_custom_target(flash
    COMMAND openocd -f "${BL_OPENOCD_CFG}" -c "program ${BL_HEX} verify reset exit"
    DEPENDS bootloader_firmware
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Flashing bootloader.hex (0x08000000) via OpenOCD"
    VERBATIM
)

add_custom_target(stflash
    COMMAND st-flash --connect-under-reset write "${BL_BIN}" 0x08000000
    DEPENDS bootloader_firmware
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Flashing bootloader.bin to 0x08000000 via st-flash"
    VERBATIM
)
