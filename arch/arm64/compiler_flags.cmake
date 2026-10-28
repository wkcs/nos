# Architecture and ABI options shared by compilation and linking.
list(APPEND NOS_GLOBAL_DEFINES "__ARM64__" "__aarch64__")
if(CONFIG_ARM64_CORTEX_A53)
    list(APPEND NOS_ARCH_FLAGS -march=armv8-a -mcpu=cortex-a53)
elseif(CONFIG_ARM64_CORTEX_A72)
    list(APPEND NOS_ARCH_FLAGS -march=armv8-a -mcpu=cortex-a72)
else()
    list(APPEND NOS_ARCH_FLAGS -march=armv8-a)
endif()
