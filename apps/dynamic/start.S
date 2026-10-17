/* Copyright (C) 2026 NOS contributors */
.text
.global _start
#if defined(__aarch64__)
_start:
    mov x0, sp
    b nos_dynamic_start
#elif defined(__riscv)
_start:
    mv a0, sp
    tail nos_dynamic_start
#else
.arm
_start:
    mov r0, sp
    b nos_dynamic_start
#endif
