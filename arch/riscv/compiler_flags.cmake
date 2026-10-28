# Architecture and ABI options shared by compilation and linking.
list(APPEND NOS_GLOBAL_DEFINES "__RISCV__")
if(CONFIG_RISCV_32IMAC)
    list(APPEND NOS_ARCH_FLAGS -march=rv32imac -mabi=ilp32)
else()
    list(APPEND NOS_ARCH_FLAGS -march=rv64gc -mabi=lp64d -mcmodel=medany)
endif()
