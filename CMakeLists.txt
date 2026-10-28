cmake_minimum_required(VERSION 3.20)

# Default to arm-none-eabi toolchain when no toolchain file was explicitly specified
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains/arm-none-eabi.cmake"
        CACHE FILEPATH "NOS Cross-Compilation Toolchain File")
endif()

project(nos VERSION 1.0.0 LANGUAGES C ASM)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/kconfig.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/compiler_flags.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/soc/chip_flags.cmake")
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

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/firmware.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/utility_targets.cmake")
