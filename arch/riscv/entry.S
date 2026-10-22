/* Copyright (C) 2026 NOS contributors */
.option nopic
.section .boot,"ax"
.global _start
_start:
    mv tp, a0
    li t0, NOS_SMP
    bgeu a0, t0, park
    la sp, kernel_stack_guard
    addi t0, a0, 1
    slli t0, t0, 15
    add sp, sp, t0
    bnez a0, secondary_wait
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sb zero, 0(t0)
    addi t0, t0, 1
    j 1b
2:  j machine_setup
secondary_wait:
    la t0, secondary_release
    ld t1, 0(t0)
    beqz t1, secondary_wait
    fence rw, rw
machine_setup:
    li t0, -1
    srli t0, t0, 2
    csrw pmpaddr0, t0
    li t0, 31
    csrw pmpcfg0, t0
    li t0, 0xb10c
    csrw medeleg, t0
    li t0, 0x222
    csrw mideleg, t0
    li t0, 7
    csrw mcounteren, t0
    csrw scounteren, t0
    la t0, machine_trap
    csrw mtvec, t0
    la t0, machine_stack_guard
    addi t1, tp, 1
    slli t1, t1, 12
    add t0, t0, t1
    csrw mscratch, t0
    la t0, supervisor_trap
    csrw stvec, t0
    li t0, 0x0200bff8
    ld t1, 0(t0)
    li t2, 100000
    add t1, t1, t2
    li t0, 0x02004000
    slli t2, tp, 3
    add t0, t0, t2
    sd t1, 0(t0)
    li t0, 136
    csrw mie, t0
    li t0, 0x6880
    csrw mstatus, t0
    la t0, supervisor_start
    csrw mepc, t0
    mret
supervisor_start:
    li t0, 2
    csrw sie, t0
    bnez tp, secondary_supervisor
    call kernel_start
secondary_supervisor:
    call kernel_secondary_start
park:
    wfi
    j park
.text
.balign 4
machine_trap:
    csrrw sp, mscratch, sp
    addi sp, sp, -32
    sd t0, 0(sp)
    sd t1, 8(sp)
    sd t2, 16(sp)
    csrr t0, mcause
    bgez t0, machine_fault
    slli t0, t0, 1
    srli t0, t0, 1
    li t1, 3
    beq t0, t1, machine_ipi
    li t1, 7
    bne t0, t1, machine_fault
    li t0, 0x0200bff8
    ld t1, 0(t0)
    li t2, 100000
    add t1, t1, t2
    li t0, 0x02004000
    csrr t2, mhartid
    slli t2, t2, 3
    add t0, t0, t2
    sd t1, 0(t0)
    li t0, 2
    csrs mip, t0
machine_return:
    ld t0, 0(sp)
    ld t1, 8(sp)
    ld t2, 16(sp)
    addi sp, sp, 32
    csrrw sp, mscratch, sp
    mret
machine_ipi:
    sfence.vma
    fence.i
    csrr t2, mhartid
    slli t2, t2, 2
    li t0, 0x02000000
    add t0, t0, t2
    sw zero, 0(t0)
    la t0, tlb_ack
    add t0, t0, t2
    li t1, 1
    fence rw, rw
    sw t1, 0(t0)
    j machine_return
machine_fault:
    la a0, machine_error
    call kernel_fail
.section .rodata
machine_error:
    .asciz "privileged_exception"
.text
.balign 4
supervisor_trap:
    /* sscratch is zero while executing supervisor code.  Never trust a
     * user stack as a kernel stack on a nested supervisor exception. */
    csrrw sp, sscratch, sp
    beqz sp, supervisor_fault
    addi sp, sp, -544
    sd zero, 0(sp)
    sd ra, 8(sp)
    sd gp, 24(sp)
    sd tp, 32(sp)
    la tp, kernel_stack_guard
    sub tp, sp, tp
    srli tp, tp, 15
    sd t0, 40(sp)
    sd t1, 48(sp)
    sd t2, 56(sp)
    sd s0, 64(sp)
    sd s1, 72(sp)
    sd a0, 80(sp)
    sd a1, 88(sp)
    sd a2, 96(sp)
    sd a3, 104(sp)
    sd a4, 112(sp)
    sd a5, 120(sp)
    sd a6, 128(sp)
    sd a7, 136(sp)
    sd s2, 144(sp)
    sd s3, 152(sp)
    sd s4, 160(sp)
    sd s5, 168(sp)
    sd s6, 176(sp)
    sd s7, 184(sp)
    sd s8, 192(sp)
    sd s9, 200(sp)
    sd s10, 208(sp)
    sd s11, 216(sp)
    sd t3, 224(sp)
    sd t4, 232(sp)
    sd t5, 240(sp)
    sd t6, 248(sp)
    fsd f0, 272(sp)
    fsd f1, 280(sp)
    fsd f2, 288(sp)
    fsd f3, 296(sp)
    fsd f4, 304(sp)
    fsd f5, 312(sp)
    fsd f6, 320(sp)
    fsd f7, 328(sp)
    fsd f8, 336(sp)
    fsd f9, 344(sp)
    fsd f10, 352(sp)
    fsd f11, 360(sp)
    fsd f12, 368(sp)
    fsd f13, 376(sp)
    fsd f14, 384(sp)
    fsd f15, 392(sp)
    fsd f16, 400(sp)
    fsd f17, 408(sp)
    fsd f18, 416(sp)
    fsd f19, 424(sp)
    fsd f20, 432(sp)
    fsd f21, 440(sp)
    fsd f22, 448(sp)
    fsd f23, 456(sp)
    fsd f24, 464(sp)
    fsd f25, 472(sp)
    fsd f26, 480(sp)
    fsd f27, 488(sp)
    fsd f28, 496(sp)
    fsd f29, 504(sp)
    fsd f30, 512(sp)
    fsd f31, 520(sp)
    frcsr t0
    sd t0, 528(sp)
    csrr t0, sscratch
    sd t0, 16(sp)
    csrw sscratch, zero
    csrr t0, sepc
    sd t0, 256(sp)
    csrr t0, sstatus
    sd t0, 264(sp)
    csrr t0, scause
    li a1, 1
    bltz t0, dispatch_trap
    li t1, 8
    li a1, 0
    beq t0, t1, dispatch_trap
    li a1, 2
dispatch_trap:
    mv a0, sp
    call kernel_trap
    addi sp, sp, 544
    j restore
supervisor_fault:
    csrr sp, sscratch
    csrw sscratch, zero
    la a0, supervisor_error
    call kernel_fail
.section .rodata
supervisor_error:
    .asciz "privileged_exception"
.text
.global arch_enter_user
arch_enter_user:
restore:
    mv t6, a0
    la t0, kernel_stack_guard
    addi t1, tp, 1
    slli t1, t1, 15
    add t0, t0, t1
    csrw sscratch, t0
    ld t0, 256(t6)
    csrw sepc, t0
    ld t0, 264(t6)
    csrw sstatus, t0
    fld f0, 272(t6)
    fld f1, 280(t6)
    fld f2, 288(t6)
    fld f3, 296(t6)
    fld f4, 304(t6)
    fld f5, 312(t6)
    fld f6, 320(t6)
    fld f7, 328(t6)
    fld f8, 336(t6)
    fld f9, 344(t6)
    fld f10, 352(t6)
    fld f11, 360(t6)
    fld f12, 368(t6)
    fld f13, 376(t6)
    fld f14, 384(t6)
    fld f15, 392(t6)
    fld f16, 400(t6)
    fld f17, 408(t6)
    fld f18, 416(t6)
    fld f19, 424(t6)
    fld f20, 432(t6)
    fld f21, 440(t6)
    fld f22, 448(t6)
    fld f23, 456(t6)
    fld f24, 464(t6)
    fld f25, 472(t6)
    fld f26, 480(t6)
    fld f27, 488(t6)
    fld f28, 496(t6)
    fld f29, 504(t6)
    fld f30, 512(t6)
    fld f31, 520(t6)
    ld t0, 528(t6)
    fscsr t0
    ld ra, 8(t6)
    ld gp, 24(t6)
    ld tp, 32(t6)
    ld t0, 40(t6)
    ld t1, 48(t6)
    ld t2, 56(t6)
    ld s0, 64(t6)
    ld s1, 72(t6)
    ld a0, 80(t6)
    ld a1, 88(t6)
    ld a2, 96(t6)
    ld a3, 104(t6)
    ld a4, 112(t6)
    ld a5, 120(t6)
    ld a6, 128(t6)
    ld a7, 136(t6)
    ld s2, 144(t6)
    ld s3, 152(t6)
    ld s4, 160(t6)
    ld s5, 168(t6)
    ld s6, 176(t6)
    ld s7, 184(t6)
    ld s8, 192(t6)
    ld s9, 200(t6)
    ld s10, 208(t6)
    ld s11, 216(t6)
    ld t3, 224(t6)
    ld t4, 232(t6)
    ld t5, 240(t6)
    ld sp, 16(t6)
    ld t6, 248(t6)
    sret
.section .bss.stack,"aw",@nobits
.balign 4096
.global kernel_stack_guard
kernel_stack_guard:
    .space NOS_SMP * 32768
.set kernel_stack_top, kernel_stack_guard + 32768
.balign 4096
machine_stack_guard:
    .space NOS_SMP * 4096
