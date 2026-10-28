# Architecture and ABI options shared by compilation and linking.
list(APPEND NOS_GLOBAL_DEFINES "__X86_64__")
list(APPEND NOS_ARCH_FLAGS -m64 -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie)
