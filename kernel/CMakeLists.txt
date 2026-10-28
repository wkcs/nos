set(KERNEL_SRCS
    printk/printk.c
    printk/log.c
    cpu/cpu.c
    mm/mm_block.c
    mm/mm_core.c
    mm/mm.c
    mm/mm_buddy.c
    mm/mm_node.c
    mm/mm_page.c
    sched/sched.c
    task/task.c
    cap/cap_table.c
    ipc/mutex.c
    ipc/sem.c
    ipc/msg_queue.c
    ipc/endpoint.c
    ipc/notification.c
    syscall/syscall.c
    irq/irq.c
    kernel.c
    console.c
    sleep.c
    spinlock.c
    pid.c
    boot.c
    init_levels.c
    timer.c
    panic.c
    kallsyms.c
)

if(CONFIG_TTY)
    list(APPEND KERNEL_SRCS tty/tty.c)
endif()

add_library(nos_kernel OBJECT ${KERNEL_SRCS})
nos_configure_kernel_target(nos_kernel)

if(CONFIG_LVGL)
    target_include_directories(nos_kernel PRIVATE
        "${CMAKE_SOURCE_DIR}/app/lvgl/lib"
        "${CMAKE_SOURCE_DIR}/app/lvgl"
    )
endif()
