# Preprocess and parse the board's device tree during CMake configuration.
if(CONFIG_DEVICE_TREE)
    set(NOS_DTS_SOURCE "${CMAKE_SOURCE_DIR}/board/${CONFIG_BOARD}/board.dts")
    set(NOS_DTS_PREPROCESSED "${CMAKE_BINARY_DIR}/generated/board.dts")
    set(NOS_DTS_C "${CMAKE_BINARY_DIR}/generated/devicetree.c")
    if(NOT EXISTS "${NOS_DTS_SOURCE}")
        message(FATAL_ERROR "Device tree enabled but board DTS is missing: ${NOS_DTS_SOURCE}")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
    file(GLOB_RECURSE _NOS_DTS_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/board/*.dts" "${CMAKE_SOURCE_DIR}/board/*.dtsi"
        "${CMAKE_SOURCE_DIR}/soc/*.dts" "${CMAKE_SOURCE_DIR}/soc/*.dtsi"
        "${CMAKE_SOURCE_DIR}/dts/*.dts" "${CMAKE_SOURCE_DIR}/dts/*.dtsi"
        "${CMAKE_SOURCE_DIR}/dts/*.h" "${CMAKE_SOURCE_DIR}/dts/bindings/*.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_NOS_DTS_INPUTS} "${CMAKE_SOURCE_DIR}/scripts/gen_devicetree.py"
        "${CMAKE_SOURCE_DIR}/scripts/vendor/zephyr_devicetree/dtlib.py")
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -E -P -x assembler-with-cpp -undef -nostdinc
                "-I${CMAKE_SOURCE_DIR}/dts" "-I${CMAKE_SOURCE_DIR}/board/${CONFIG_BOARD}"
                "${NOS_DTS_SOURCE}" -o "${NOS_DTS_PREPROCESSED}"
        RESULT_VARIABLE _NOS_DTS_CPP_RESULT
    )
    if(NOT _NOS_DTS_CPP_RESULT EQUAL 0)
        message(FATAL_ERROR "Device tree preprocessing failed: ${NOS_DTS_SOURCE}")
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/gen_devicetree.py"
                --input "${NOS_DTS_PREPROCESSED}" --output "${NOS_DTS_C}"
                --include-dir "${CMAKE_SOURCE_DIR}/board/${CONFIG_BOARD}"
                --include-dir "${CMAKE_SOURCE_DIR}/dts"
                --bindings "${CMAKE_SOURCE_DIR}/dts/bindings"
        RESULT_VARIABLE _NOS_DTS_RESULT
    )
    if(NOT _NOS_DTS_RESULT EQUAL 0)
        message(FATAL_ERROR "Device tree parsing failed: ${NOS_DTS_SOURCE}")
    endif()
endif()
