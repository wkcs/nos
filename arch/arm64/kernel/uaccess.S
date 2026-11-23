/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 安全用户态-内核态拷贝原语（配合 PAN 与 __ex_table 异常修复表）
 */

#define __ASSEMBLY__

    .macro ex_table_add insn_lbl, fixup_lbl
    .pushsection __ex_table, "a"
    .align 3
    .quad \insn_lbl
    .quad \fixup_lbl
    .popsection
    .endm

    .text
    .align 2

    /*
     * int __arm64_raw_copy_from_user(void *kdst, uintptr_t usrc, size_t n, uint32_t has_pan)
     * x0 = kdst, x1 = usrc, x2 = n, w3 = has_pan
     */
    .global __arm64_raw_copy_from_user
    .type __arm64_raw_copy_from_user, %function
__arm64_raw_copy_from_user:
    hint    #34
    cbz     x2, .Lcfu_done_ok
    cbz     w3, .Lcfu_qword_loop
    .inst   0xD500409F                  /* msr pan, #0 */
    isb

.Lcfu_qword_loop:
    cmp     x2, #8
    b.lo    .Lcfu_byte_loop
.Lcfu_q_insn:
    ldr     x4, [x1]
    ex_table_add .Lcfu_q_insn, .Lcfu_fault
    str     x4, [x0]
    add     x1, x1, #8
    add     x0, x0, #8
    sub     x2, x2, #8
    b       .Lcfu_qword_loop

.Lcfu_byte_loop:
    cbz     x2, .Lcfu_restore_ok
.Lcfu_b_insn:
    ldrb    w4, [x1]
    ex_table_add .Lcfu_b_insn, .Lcfu_fault
    strb    w4, [x0]
    add     x1, x1, #1
    add     x0, x0, #1
    sub     x2, x2, #1
    b       .Lcfu_byte_loop

.Lcfu_restore_ok:
    cbz     w3, .Lcfu_done_ok
    .inst   0xD500419F                  /* msr pan, #1 */
    isb
.Lcfu_done_ok:
    mov     x0, #0
    ret

.Lcfu_fault:
    cbz     w3, 1f
    .inst   0xD500419F                  /* msr pan, #1 */
    isb
1:
    mov     x0, #-14                    /* -EFAULT */
    ret
    .size __arm64_raw_copy_from_user, .-__arm64_raw_copy_from_user

    /*
     * int __arm64_raw_copy_to_user(uintptr_t udst, const void *ksrc, size_t n, uint32_t has_pan)
     * x0 = udst, x1 = ksrc, x2 = n, w3 = has_pan
     */
    .global __arm64_raw_copy_to_user
    .type __arm64_raw_copy_to_user, %function
__arm64_raw_copy_to_user:
    hint    #34
    cbz     x2, .Lctu_done_ok
    cbz     w3, .Lctu_qword_loop
    .inst   0xD500409F                  /* msr pan, #0 */
    isb

.Lctu_qword_loop:
    cmp     x2, #8
    b.lo    .Lctu_byte_loop
    ldr     x4, [x1]
.Lctu_q_insn:
    str     x4, [x0]
    ex_table_add .Lctu_q_insn, .Lctu_fault
    add     x1, x1, #8
    add     x0, x0, #8
    sub     x2, x2, #8
    b       .Lctu_qword_loop

.Lctu_byte_loop:
    cbz     x2, .Lctu_restore_ok
    ldrb    w4, [x1]
.Lctu_b_insn:
    strb    w4, [x0]
    ex_table_add .Lctu_b_insn, .Lctu_fault
    add     x1, x1, #1
    add     x0, x0, #1
    sub     x2, x2, #1
    b       .Lctu_byte_loop

.Lctu_restore_ok:
    cbz     w3, .Lctu_done_ok
    .inst   0xD500419F                  /* msr pan, #1 */
    isb
.Lctu_done_ok:
    mov     x0, #0
    ret

.Lctu_fault:
    cbz     w3, 1f
    .inst   0xD500419F                  /* msr pan, #1 */
    isb
1:
    mov     x0, #-14                    /* -EFAULT */
    ret
    .size __arm64_raw_copy_to_user, .-__arm64_raw_copy_to_user
