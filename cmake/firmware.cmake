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

