#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/kallsyms.h>

void asm_cpu_set_lpm(void)
{
    asm volatile ("wfi");
}

void arch_dump_stack(void)
{
    unsigned long *fp;
    unsigned long pc, lr;
    int depth = 0;
    
    /* 获取当前的FP (在Cortex-M上通常是r7) */
    __asm__ volatile("mov %0, r7" : "=r"(fp));
    
    pr_info("Call Trace:\r\n");
    
    /* 
     * ARM AAPCS calling convention with -mapcs-frame:
     * fp[0] -> saved fp (previous frame pointer)
     * fp[1] -> saved lr (return address)
     * fp[2] -> saved sp
     * fp[3] -> saved pc
     */
    while (depth < 16 && fp != NULL) {
        /* 简单的有效性检查，防止访问非法内存 */
        if ((unsigned long)fp < 0x20000000 || (unsigned long)fp > 0x30000000) { 
            break;
        }
        
        /* 检查是否对齐 */
        if ((unsigned long)fp & 3) {
            break;
        }

        /* 读取返回地址 */
        lr = fp[1];
        
        /* 打印调用信息，格式：[<地址>] 符号名+偏移/总大小 [模块名] */
        /* 注意：Thumb指令集地址最低位为1，查找符号时需要屏蔽 */
        print_symbol(" [<0x%08lx>] %s\r\n", lr & ~1UL);
        
        /* 移动到上一帧 */
        fp = (unsigned long *)fp[0];
        
        depth++;
    }
    pr_info("\r\n");
}
