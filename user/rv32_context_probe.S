/* Copyright (C) 2026 NOS contributors */
.text
.global nos_fp_set
.type nos_fp_set,@function
nos_fp_set:
    addi sp, sp, -16
    sw a0, 0(sp)
    sw a1, 4(sp)
    fld fs0, 0(sp)
    addi sp, sp, 16
    ret
.global nos_fp_get
.type nos_fp_get,@function
nos_fp_get:
    addi sp, sp, -16
    fsd fs0, 0(sp)
    lw a0, 0(sp)
    lw a1, 4(sp)
    addi sp, sp, 16
    ret
