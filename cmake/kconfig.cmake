#
# NOS CMake Kconfig Integration Module
# Invokes scripts/gen_kconfig_cmake.py during CMake configure phase and imports CONFIG_* variables.
#

find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT DEFINED NOS_DEFCONFIG OR "${NOS_DEFCONFIG}" STREQUAL "")
    set(NOS_DEFCONFIG "${CMAKE_SOURCE_DIR}/configs/nk60-v2_defconfig" CACHE FILEPATH "Path to base NOS defconfig" FORCE)
endif()

set(NOS_CONFIG_FRAGMENTS "" CACHE STRING "Optional semicolon-separated list of Kconfig fragment files")

set(_KCONFIG_INPUTS "${NOS_DEFCONFIG}")
foreach(_frag IN LISTS NOS_CONFIG_FRAGMENTS)
    if(NOT "${_frag}" STREQUAL "")
        list(APPEND _KCONFIG_INPUTS "${_frag}")
    endif()
endforeach()

# Track Kconfig files so CMake automatically re-runs configure when Kconfig or defconfig changes
file(GLOB_RECURSE _KCONFIG_TREE_FILES
    "${CMAKE_SOURCE_DIR}/Kconfig"
    "${CMAKE_SOURCE_DIR}/arch/*Kconfig"
    "${CMAKE_SOURCE_DIR}/soc/*Kconfig"
    "${CMAKE_SOURCE_DIR}/board/*Kconfig"
    "${CMAKE_SOURCE_DIR}/drivers/*Kconfig"
    "${CMAKE_SOURCE_DIR}/fs/*Kconfig"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_KCONFIG_TREE_FILES}
    ${_KCONFIG_INPUTS}
    "${CMAKE_SOURCE_DIR}/scripts/gen_kconfig_cmake.py"
)

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/gen_kconfig_cmake.py"
            --srctree "${CMAKE_SOURCE_DIR}"
            --out-dir "${CMAKE_BINARY_DIR}"
            --version "${PROJECT_VERSION}"
            ${_KCONFIG_INPUTS}
    RESULT_VARIABLE _KCONFIG_RET
)

if(NOT _KCONFIG_RET EQUAL 0)
    message(FATAL_ERROR "Failed to generate Kconfig configuration from: ${_KCONFIG_INPUTS}")
endif()

include("${CMAKE_BINARY_DIR}/kconfig.cmake")
