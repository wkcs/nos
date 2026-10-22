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
    lw t1, 0(t0)
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
1:  lw t2, 4(t0)
    lw t1, 0(t0)
    lw t3, 4(t0)
    bne t2, t3, 1b
    li t3, 100000
    add t3, t1, t3
    sltu t4, t3, t1
    add t2, t2, t4
    li t0, 0x02004000
    slli t4, tp, 3
    add t0, t0, t4
    li t4, -1
    sw t4, 0(t0)
    sw t2, 4(t0)
    sw t3, 0(t0)
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
    sw t0, 0(sp)
    sw t1, 4(sp)
    sw t2, 8(sp)
    sw t3, 12(sp)
    sw t4, 16(sp)
    csrr t0, mcause
    bgez t0, machine_fault
    slli t0, t0, 1
    srli t0, t0, 1
    li t1, 3
    beq t0, t1, machine_ipi
    li t1, 7
    bne t0, t1, machine_fault
    li t0, 0x0200bff8
1:  lw t2, 4(t0)
    lw t1, 0(t0)
    lw t3, 4(t0)
    bne t2, t3, 1b
    li t3, 100000
    add t3, t1, t3
    sltu t4, t3, t1
    add t2, t2, t4
    li t0, 0x02004000
    csrr t4, mhartid
    slli t4, t4, 3
    add t0, t0, t4
    li t4, -1
    sw t4, 0(t0)
    sw t2, 4(t0)
    sw t3, 0(t0)
    li t0, 2
    csrs mip, t0
machine_return:
    lw t0, 0(sp)
    lw t1, 4(sp)
    lw t2, 8(sp)
    lw t3, 12(sp)
    lw t4, 16(sp)
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
    addi sp, sp, -416
    sw zero, 0(sp)
    sw ra, 4(sp)
    sw gp, 12(sp)
    sw tp, 16(sp)
    la tp, kernel_stack_guard
    sub tp, sp, tp
    srli tp, tp, 15
    sw t0, 20(sp)
    sw t1, 24(sp)
    sw t2, 28(sp)
    sw s0, 32(sp)
    sw s1, 36(sp)
    sw a0, 40(sp)
    sw a1, 44(sp)
    sw a2, 48(sp)
    sw a3, 52(sp)
    sw a4, 56(sp)
    sw a5, 60(sp)
    sw a6, 64(sp)
    sw a7, 68(sp)
    sw s2, 72(sp)
    sw s3, 76(sp)
    sw s4, 80(sp)
    sw s5, 84(sp)
    sw s6, 88(sp)
    sw s7, 92(sp)
    sw s8, 96(sp)
    sw s9, 100(sp)
    sw s10, 104(sp)
    sw s11, 108(sp)
    sw t3, 112(sp)
    sw t4, 116(sp)
    sw t5, 120(sp)
    sw t6, 124(sp)
    fsd f0, 136(sp)
    fsd f1, 144(sp)
    fsd f2, 152(sp)
    fsd f3, 160(sp)
    fsd f4, 168(sp)
    fsd f5, 176(sp)
    fsd f6, 184(sp)
    fsd f7, 192(sp)
    fsd f8, 200(sp)
    fsd f9, 208(sp)
    fsd f10, 216(sp)
    fsd f11, 224(sp)
    fsd f12, 232(sp)
    fsd f13, 240(sp)
    fsd f14, 248(sp)
    fsd f15, 256(sp)
    fsd f16, 264(sp)
    fsd f17, 272(sp)
    fsd f18, 280(sp)
    fsd f19, 288(sp)
    fsd f20, 296(sp)
    fsd f21, 304(sp)
    fsd f22, 312(sp)
    fsd f23, 320(sp)
    fsd f24, 328(sp)
    fsd f25, 336(sp)
    fsd f26, 344(sp)
    fsd f27, 352(sp)
    fsd f28, 360(sp)
    fsd f29, 368(sp)
    fsd f30, 376(sp)
    fsd f31, 384(sp)
    frcsr t0
    sw t0, 392(sp)
    csrr t0, sscratch
    sw t0, 8(sp)
    csrw sscratch, zero
    csrr t0, sepc
    sw t0, 128(sp)
    csrr t0, sstatus
    sw t0, 132(sp)
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
    addi sp, sp, 416
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
    lw t0, 128(t6)
    csrw sepc, t0
    lw t0, 132(t6)
    csrw sstatus, t0
    fld f0, 136(t6)
    fld f1, 144(t6)
    fld f2, 152(t6)
    fld f3, 160(t6)
    fld f4, 168(t6)
    fld f5, 176(t6)
    fld f6, 184(t6)
    fld f7, 192(t6)
    fld f8, 200(t6)
    fld f9, 208(t6)
    fld f10, 216(t6)
    fld f11, 224(t6)
    fld f12, 232(t6)
    fld f13, 240(t6)
    fld f14, 248(t6)
    fld f15, 256(t6)
    fld f16, 264(t6)
    fld f17, 272(t6)
    fld f18, 280(t6)
    fld f19, 288(t6)
    fld f20, 296(t6)
    fld f21, 304(t6)
    fld f22, 312(t6)
    fld f23, 320(t6)
    fld f24, 328(t6)
    fld f25, 336(t6)
    fld f26, 344(t6)
    fld f27, 352(t6)
    fld f28, 360(t6)
    fld f29, 368(t6)
    fld f30, 376(t6)
    fld f31, 384(t6)
    lw t0, 392(t6)
    fscsr t0
    lw ra, 4(t6)
    lw gp, 12(t6)
    lw tp, 16(t6)
    lw t0, 20(t6)
    lw t1, 24(t6)
    lw t2, 28(t6)
    lw s0, 32(t6)
    lw s1, 36(t6)
    lw a0, 40(t6)
    lw a1, 44(t6)
    lw a2, 48(t6)
    lw a3, 52(t6)
    lw a4, 56(t6)
    lw a5, 60(t6)
    lw a6, 64(t6)
    lw a7, 68(t6)
    lw s2, 72(t6)
    lw s3, 76(t6)
    lw s4, 80(t6)
    lw s5, 84(t6)
    lw s6, 88(t6)
    lw s7, 92(t6)
    lw s8, 96(t6)
    lw s9, 100(t6)
    lw s10, 104(t6)
    lw s11, 108(t6)
    lw t3, 112(t6)
    lw t4, 116(t6)
    lw t5, 120(t6)
    lw sp, 8(t6)
    lw t6, 124(t6)
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
