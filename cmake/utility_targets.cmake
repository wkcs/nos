# ------------------------------------------------------------------------------
# Cross-Platform Utility Targets (Flash, Debug, QEMU, Menuconfig)
# ------------------------------------------------------------------------------
set(BOARD_OPENOCD_CFG "${CMAKE_CURRENT_SOURCE_DIR}/board/${CONFIG_BOARD}/openocd.cfg")
set(NOS_OPENOCD "openocd" CACHE STRING "OpenOCD executable name or absolute path")
set(NOS_OPENOCD_SCRIPTS "" CACHE PATH "OpenOCD script directory")
set(_NOS_OPENOCD_ARGS "")
if(NOS_OPENOCD_SCRIPTS)
    list(APPEND _NOS_OPENOCD_ARGS -s "${NOS_OPENOCD_SCRIPTS}")
endif()

add_custom_target(flash
    COMMAND "${NOS_OPENOCD}" ${_NOS_OPENOCD_ARGS} -f "${BOARD_OPENOCD_CFG}" -c "program ${CMAKE_CURRENT_BINARY_DIR}/nos.hex verify reset exit"
    DEPENDS nos_firmware
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Flashing nos.hex via OpenOCD"
    VERBATIM
)

add_custom_target(stflash
    COMMAND st-flash --connect-under-reset write "${CMAKE_CURRENT_BINARY_DIR}/nos.bin" "${CONFIG_KERNEL_ADDR}"
    DEPENDS nos_firmware
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Flashing nos.bin to ${CONFIG_KERNEL_ADDR} via st-flash"
    VERBATIM
)

if("${CONFIG_BOARD}" STREQUAL "qemu-stm32")
    add_custom_target(qemu-run
        COMMAND qemu-system-arm -M netduinoplus2 -nographic -semihosting -semihosting-config enable=on,target=native -kernel "$<TARGET_FILE:nos.elf>"
        DEPENDS nos.elf
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Running nos.elf in QEMU (netduinoplus2)"
        VERBATIM
    )
    add_custom_target(qemu-run-gdb
        COMMAND qemu-system-arm -M netduinoplus2 -nographic -semihosting -semihosting-config enable=on,target=native -kernel "$<TARGET_FILE:nos.elf>" -S -s
        DEPENDS nos.elf
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
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
