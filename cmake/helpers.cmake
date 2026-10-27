#
# NOS CMake Helper Functions for Kernel Subsystems and User-Space Services
#

# Apply standard kernel compilation flags, defines, and header paths to an OBJECT or STATIC target
function(nos_configure_kernel_target TARGET_NAME)
    target_link_libraries(${TARGET_NAME} PRIVATE nos_headers)
    target_compile_definitions(${TARGET_NAME} PRIVATE ${NOS_GLOBAL_DEFINES})
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${NOS_C_FLAGS}>
        $<$<COMPILE_LANGUAGE:ASM>:${NOS_ASM_FLAGS}>
    )
endfunction()

# Define an isolated User-Space Service / Application module (for Microkernel SASOS/MMU mode)
function(nos_add_user_service TARGET_NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;DEFINES" ${ARGN})
    add_library(${TARGET_NAME} OBJECT ${ARG_SOURCES})
    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/include/uapi"
        "${CMAKE_BINARY_DIR}/include"
        ${ARG_INCLUDES}
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE ${NOS_GLOBAL_DEFINES} "__NOS_USER__" ${ARG_DEFINES})
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${NOS_C_FLAGS}>
        $<$<COMPILE_LANGUAGE:ASM>:${NOS_ASM_FLAGS}>
    )
endfunction()
