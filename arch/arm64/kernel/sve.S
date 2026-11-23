/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64/ARMv9 SVE 变长矢量上下文保存、恢复与清零汇编原语
 * 独立声明 .arch armv8-a+sve，由运行期 has_sve 硬件探测门控调用
 */

    .arch armv8-a+sve
    .text
    .align 2

    .global arm64_sve_rdvl
    .type arm64_sve_rdvl, %function
arm64_sve_rdvl:
    hint    #34
    rdvl    x0, #1
    ret
    .size arm64_sve_rdvl, .-arm64_sve_rdvl

    /*
     * x0 = struct sve_buf * (16B 对齐): [Z0..Z31 | P0..P15 | FFR | align16 | FPSR/FPCR]
     * w1 = vl (有效矢量字节长度，如 16 / 32 / 64)
     */
    .global arm64_sve_save
    .type arm64_sve_save, %function
arm64_sve_save:
    hint    #34
    uxtw    x1, w1
    str     z0,  [x0, #0,  mul vl]
    str     z1,  [x0, #1,  mul vl]
    str     z2,  [x0, #2,  mul vl]
    str     z3,  [x0, #3,  mul vl]
    str     z4,  [x0, #4,  mul vl]
    str     z5,  [x0, #5,  mul vl]
    str     z6,  [x0, #6,  mul vl]
    str     z7,  [x0, #7,  mul vl]
    str     z8,  [x0, #8,  mul vl]
    str     z9,  [x0, #9,  mul vl]
    str     z10, [x0, #10, mul vl]
    str     z11, [x0, #11, mul vl]
    str     z12, [x0, #12, mul vl]
    str     z13, [x0, #13, mul vl]
    str     z14, [x0, #14, mul vl]
    str     z15, [x0, #15, mul vl]
    str     z16, [x0, #16, mul vl]
    str     z17, [x0, #17, mul vl]
    str     z18, [x0, #18, mul vl]
    str     z19, [x0, #19, mul vl]
    str     z20, [x0, #20, mul vl]
    str     z21, [x0, #21, mul vl]
    str     z22, [x0, #22, mul vl]
    str     z23, [x0, #23, mul vl]
    str     z24, [x0, #24, mul vl]
    str     z25, [x0, #25, mul vl]
    str     z26, [x0, #26, mul vl]
    str     z27, [x0, #27, mul vl]
    str     z28, [x0, #28, mul vl]
    str     z29, [x0, #29, mul vl]
    str     z30, [x0, #30, mul vl]
    str     z31, [x0, #31, mul vl]

    add     x9, x0, x1, lsl #5      /* + 32 * vl */
    str     p0,  [x9, #0,  mul vl]
    str     p1,  [x9, #1,  mul vl]
    str     p2,  [x9, #2,  mul vl]
    str     p3,  [x9, #3,  mul vl]
    str     p4,  [x9, #4,  mul vl]
    str     p5,  [x9, #5,  mul vl]
    str     p6,  [x9, #6,  mul vl]
    str     p7,  [x9, #7,  mul vl]
    str     p8,  [x9, #8,  mul vl]
    str     p9,  [x9, #9,  mul vl]
    str     p10, [x9, #10, mul vl]
    str     p11, [x9, #11, mul vl]
    str     p12, [x9, #12, mul vl]
    str     p13, [x9, #13, mul vl]
    str     p14, [x9, #14, mul vl]
    str     p15, [x9, #15, mul vl]

    add     x10, x9, x1, lsl #1     /* + 2 * vl -> FFR 槽 */
    rdffr   p0.b
    str     p0, [x10]
    ldr     p0, [x9, #0, mul vl]

    /* 计算 16B 对齐后的 FPSR/FPCR 尾部偏移 */
    lsl     x10, x1, #5
    add     x10, x10, x1, lsl #1
    add     x10, x10, x1, lsr #3
    add     x10, x10, #15
    bic     x10, x10, #15
    add     x11, x0, x10

    mrs     x8, fpsr
    str     w8, [x11, #0]
    mrs     x8, fpcr
    str     w8, [x11, #4]
    ret
    .size arm64_sve_save, .-arm64_sve_save

    .global arm64_sve_restore
    .type arm64_sve_restore, %function
arm64_sve_restore:
    hint    #34
    uxtw    x1, w1
    add     x9, x0, x1, lsl #5
    add     x10, x9, x1, lsl #1
    ldr     p0, [x10]
    wrffr   p0.b

    ldr     p0,  [x9, #0,  mul vl]
    ldr     p1,  [x9, #1,  mul vl]
    ldr     p2,  [x9, #2,  mul vl]
    ldr     p3,  [x9, #3,  mul vl]
    ldr     p4,  [x9, #4,  mul vl]
    ldr     p5,  [x9, #5,  mul vl]
    ldr     p6,  [x9, #6,  mul vl]
    ldr     p7,  [x9, #7,  mul vl]
    ldr     p8,  [x9, #8,  mul vl]
    ldr     p9,  [x9, #9,  mul vl]
    ldr     p10, [x9, #10, mul vl]
    ldr     p11, [x9, #11, mul vl]
    ldr     p12, [x9, #12, mul vl]
    ldr     p13, [x9, #13, mul vl]
    ldr     p14, [x9, #14, mul vl]
    ldr     p15, [x9, #15, mul vl]

    ldr     z0,  [x0, #0,  mul vl]
    ldr     z1,  [x0, #1,  mul vl]
    ldr     z2,  [x0, #2,  mul vl]
    ldr     z3,  [x0, #3,  mul vl]
    ldr     z4,  [x0, #4,  mul vl]
    ldr     z5,  [x0, #5,  mul vl]
    ldr     z6,  [x0, #6,  mul vl]
    ldr     z7,  [x0, #7,  mul vl]
    ldr     z8,  [x0, #8,  mul vl]
    ldr     z9,  [x0, #9,  mul vl]
    ldr     z10, [x0, #10, mul vl]
    ldr     z11, [x0, #11, mul vl]
    ldr     z12, [x0, #12, mul vl]
    ldr     z13, [x0, #13, mul vl]
    ldr     z14, [x0, #14, mul vl]
    ldr     z15, [x0, #15, mul vl]
    ldr     z16, [x0, #16, mul vl]
    ldr     z17, [x0, #17, mul vl]
    ldr     z18, [x0, #18, mul vl]
    ldr     z19, [x0, #19, mul vl]
    ldr     z20, [x0, #20, mul vl]
    ldr     z21, [x0, #21, mul vl]
    ldr     z22, [x0, #22, mul vl]
    ldr     z23, [x0, #23, mul vl]
    ldr     z24, [x0, #24, mul vl]
    ldr     z25, [x0, #25, mul vl]
    ldr     z26, [x0, #26, mul vl]
    ldr     z27, [x0, #27, mul vl]
    ldr     z28, [x0, #28, mul vl]
    ldr     z29, [x0, #29, mul vl]
    ldr     z30, [x0, #30, mul vl]
    ldr     z31, [x0, #31, mul vl]

    lsl     x10, x1, #5
    add     x10, x10, x1, lsl #1
    add     x10, x10, x1, lsr #3
    add     x10, x10, #15
    bic     x10, x10, #15
    add     x11, x0, x10

    ldr     w8, [x11, #0]
    msr     fpsr, x8
    ldr     w8, [x11, #4]
    msr     fpcr, x8
    ret
    .size arm64_sve_restore, .-arm64_sve_restore

    .global arm64_sve_zero
    .type arm64_sve_zero, %function
arm64_sve_zero:
    hint    #34
    dup     z0.d,  #0
    dup     z1.d,  #0
    dup     z2.d,  #0
    dup     z3.d,  #0
    dup     z4.d,  #0
    dup     z5.d,  #0
    dup     z6.d,  #0
    dup     z7.d,  #0
    dup     z8.d,  #0
    dup     z9.d,  #0
    dup     z10.d, #0
    dup     z11.d, #0
    dup     z12.d, #0
    dup     z13.d, #0
    dup     z14.d, #0
    dup     z15.d, #0
    dup     z16.d, #0
    dup     z17.d, #0
    dup     z18.d, #0
    dup     z19.d, #0
    dup     z20.d, #0
    dup     z21.d, #0
    dup     z22.d, #0
    dup     z23.d, #0
    dup     z24.d, #0
    dup     z25.d, #0
    dup     z26.d, #0
    dup     z27.d, #0
    dup     z28.d, #0
    dup     z29.d, #0
    dup     z30.d, #0
    dup     z31.d, #0

    pfalse  p0.b
    pfalse  p1.b
    pfalse  p2.b
    pfalse  p3.b
    pfalse  p4.b
    pfalse  p5.b
    pfalse  p6.b
    pfalse  p7.b
    pfalse  p8.b
    pfalse  p9.b
    pfalse  p10.b
    pfalse  p11.b
    pfalse  p12.b
    pfalse  p13.b
    pfalse  p14.b
    pfalse  p15.b
    wrffr   p0.b

    msr     fpsr, xzr
    msr     fpcr, xzr
    ret
    .size arm64_sve_zero, .-arm64_sve_zero
