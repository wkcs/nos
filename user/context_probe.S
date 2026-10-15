/* Copyright (C) 2026 NOS contributors. Test-only register accessors. */
.text
#ifdef NOS_ARM64
.global nos_fp_set
nos_fp_set:
    fmov d8, x0
    ret
.global nos_fp_get
nos_fp_get:
    fmov x0, d8
    ret
#endif
#ifdef NOS_RISCV
.global nos_fp_set
nos_fp_set:
    fmv.d.x fs0, a0
    ret
.global nos_fp_get
nos_fp_get:
    fmv.x.d a0, fs0
    ret
#endif
