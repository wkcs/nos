/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

.global  g_pfnVectors
.global  Default_Handler
.global  reset_handler

/* 启动时的数据段初始化值地址，在链接脚本中定义 */
.word  _sidata
/* .data 段的起始地址，在链接脚本中定义 */
.word  _sdata
/* .data 段的结束地址，在链接脚本中定义 */
.word  _edata
/* .bss 段的起始地址，在链接脚本中定义 */
.word  _sbss
/* .bss 段的结束地址，在链接脚本中定义 */
.word  _ebss

/**
 * @brief  处理器复位后首先执行的代码
 *         只执行绝对必要的初始化，然后调用应用程序提供的 main() 函数
 * @param  None
 * @retval : None
 */
    .section  .text.reset_handler
    .weak  reset_handler
    .type  reset_handler, %function
reset_handler:

/* 将数据段初始化值从 flash 复制到 SRAM */
    movs  r1, #0
    b  LoopCopyDataInit

CopyDataInit:
    ldr  r3, =_sidata
    ldr  r3, [r3, r1]
    str  r3, [r0, r1]
    adds  r1, r1, #4

LoopCopyDataInit:
    ldr  r0, =_sdata
    ldr  r3, =_edata
    adds  r2, r0, r1
    cmp  r2, r3
    bcc  CopyDataInit
    ldr  r2, =_sbss
    b  LoopFillZerobss

/* 将 bss 段清零 */
FillZerobss:
    movs  r3, #0
    str  r3, [r2], #4

LoopFillZerobss:
    ldr  r3, = _ebss
    cmp  r2, r3
    bcc  FillZerobss

/* 调用系统初始化函数 */
    bl  SystemInit
/* 调用静态构造函数 */
    bl __libc_init_array
/* 调用应用程序入口点 */
    bl  main
    bx  lr
.size  reset_handler, .-reset_handler

/**
 * @brief  默认中断处理程序
 */
    .section  .text.Default_Handler,"ax",%progbits
Default_Handler:
Infinite_Loop:
    b  Infinite_Loop
    .size  Default_Handler, .-Default_Handler