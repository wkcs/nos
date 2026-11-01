/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/unwind.h>
#include <kernel/kallsyms.h>
#include <kernel/printk.h>
#include <kernel/errno.h>
#include <nos/sched.h>
#include <lib/vsprintf.h>
#include <stdarg.h>
#include <string.h>

#ifdef CONFIG_SMALL_MEMORY
#define DUMP_LOCAL_MAX_FRAMES 6U
#elif defined(CONFIG_BACKTRACE_MAX_DEPTH) && (CONFIG_BACKTRACE_MAX_DEPTH <= 16)
#define DUMP_LOCAL_MAX_FRAMES ((size_t)CONFIG_BACKTRACE_MAX_DEPTH)
#else
#define DUMP_LOCAL_MAX_FRAMES 16U
#endif

static int default_dump_print(const char *fmt, ...)
{
    va_list args;
    int rc;

    va_start(args, fmt);
    rc = vprintk(fmt, args);
    va_end(args);
    return rc;
}

__attribute__((weak)) const char *task_state_short_str(uint8_t status)
{
    switch (status) {
    case TASK_RUNNING:
    case TASK_READY:
        return "R";
    case TASK_SLEEPING:
        return "S";
    case TASK_BLOCKED:
        return "D";
    case TASK_SUSPENDED:
        return "T";
    case TASK_ZOMBIE:
        return "Z";
    default:
        return "?";
    }
}

__attribute__((weak)) const char *task_policy_short_str(uint8_t policy)
{
    switch (policy) {
    case SCHED_POLICY_RR:
        return "RR";
    case SCHED_POLICY_FIFO:
        return "FF";
    case SCHED_POLICY_NORMAL:
        return "TS";
    case SCHED_POLICY_IDLE:
        return "IDL";
    default:
        return "?";
    }
}

__attribute__((weak)) void task_get_stack_bounds(struct task_struct *task,
                                                 uintptr_t *low,
                                                 uintptr_t *high)
{
    if (low != NULL) {
        *low = 0U;
    }
    if (high != NULL) {
        *high = 0U;
    }
    if (task == NULL || task->stack == NULL || task->stack_size == 0U) {
        return;
    }
#ifdef CONFIG_STACK_GROWSUP
    if (low != NULL) {
        *low = (uintptr_t)task->stack;
    }
    if (high != NULL) {
        *high = (uintptr_t)task->stack + (uintptr_t)task->stack_size;
    }
#else
    if (low != NULL) {
        *low = (uintptr_t)task->stack - (uintptr_t)task->stack_size + sizeof(addr_t);
    }
    if (high != NULL) {
        *high = (uintptr_t)task->stack + sizeof(addr_t);
    }
#endif
}

__attribute__((weak)) bool task_stack_is_healthy(struct task_struct *task)
{
    uintptr_t low = 0U;
    uintptr_t high = 0U;

    if (task == NULL || task->stack == NULL || task->stack_size == 0U) {
        return false;
    }
    task_get_stack_bounds(task, &low, &high);
    if (low == 0U || high <= low) {
        return false;
    }
#ifdef CONFIG_STACK_GROWSUP
    if (*(const char *)(high - 1U) != '#') {
        return false;
    }
#else
    if (*(const char *)low != '#') {
        return false;
    }
#endif
    if (task->sp != NULL && ((uintptr_t)task->sp < low || (uintptr_t)task->sp > high)) {
        return false;
    }
    return true;
}

int kallsyms_sprint_symbol(char *buf, size_t size, unsigned long addr)
{
    const char *name;
    unsigned long symsize = 0UL;
    unsigned long offset = 0UL;

    if (buf == NULL || size == 0U) {
        return 0;
    }

    name = kallsyms_lookup(addr, &symsize, &offset, NULL, NULL);
    if (name != NULL) {
        if (symsize > 0UL) {
            return snprintf(buf, size, "%s+0x%lx/0x%lx", name, offset, symsize);
        }
        return snprintf(buf, size, "%s+0x%lx", name, offset);
    }
    return snprintf(buf, size, "0x%08lx", addr);
}

static bool addr_is_valid_text(uintptr_t addr,
                               uintptr_t text_start,
                               uintptr_t text_end,
                               bool require_offset)
{
    uintptr_t code_addr = addr & ~(uintptr_t)1U;
    unsigned long off = 0UL;
    const char *sym;

    if (code_addr == 0U) {
        return false;
    }
    if (text_start != 0U && text_end > text_start) {
        return code_addr >= text_start && code_addr < text_end;
    }
    sym = kallsyms_lookup((unsigned long)code_addr, NULL, &off, NULL, NULL);
    if (sym == NULL) {
        return false;
    }
    if (require_offset && off == 0UL) {
        return false;
    }
    return true;
}

static bool thumb_is_preceded_by_bl(uintptr_t val, uintptr_t text_start)
{
    uintptr_t code_addr = val & ~(uintptr_t)1U;

    if ((val & 1U) == 0U) {
        return false;
    }
    if (code_addr >= text_start + 4U) {
        uint16_t hw1 = *(const uint16_t *)(code_addr - 4U);
        uint16_t hw2 = *(const uint16_t *)(code_addr - 2U);
        if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0xC000U) == 0xC000U) {
            return true;
        }
    }
    if (code_addr >= text_start + 2U) {
        uint16_t hw = *(const uint16_t *)(code_addr - 2U);
        if ((hw & 0xFF80U) == 0x4780U) {
            return true;
        }
    }
    return false;
}

static size_t push_unique_pc(uintptr_t *out_pcs,
                             size_t count,
                             size_t max_frames,
                             uintptr_t pc)
{
    uintptr_t clean_pc = pc & ~(uintptr_t)1U;

    if (clean_pc == 0U || count >= max_frames) {
        return count;
    }
    if (count > 0U && out_pcs[count - 1U] == clean_pc) {
        return count;
    }
    out_pcs[count] = clean_pc;
    return count + 1U;
}

size_t unwind_frames_from_state(const struct unwind_frame *init_frame,
                                uintptr_t text_start,
                                uintptr_t text_end,
                                bool thumb_check_bl,
                                uintptr_t *out_pcs,
                                size_t max_frames)
{
    size_t count = 0U;
    size_t fp_frames = 0U;
    uintptr_t fp;
    uintptr_t scan;
    uintptr_t limit;

    if (init_frame == NULL || out_pcs == NULL || max_frames == 0U) {
        return 0U;
    }

    if (init_frame->pc != 0U &&
        addr_is_valid_text(init_frame->pc, text_start, text_end, false)) {
        count = push_unique_pc(out_pcs, count, max_frames, init_frame->pc);
    }

    /* Stage 1: Frame Pointer chain unwinding */
    fp = init_frame->fp;
    while (count < max_frames &&
           fp >= init_frame->stack_low &&
           fp + 2U * sizeof(uintptr_t) <= init_frame->stack_high &&
           (fp & (sizeof(uintptr_t) - 1U)) == 0U) {
        uintptr_t next_fp = ((const uintptr_t *)fp)[0];
        uintptr_t ret_lr = ((const uintptr_t *)fp)[1];

        if (addr_is_valid_text(ret_lr, text_start, text_end, false)) {
            size_t prev = count;
            count = push_unique_pc(out_pcs, count, max_frames, ret_lr);
            if (count > prev) {
                fp_frames++;
            }
        }
        if (next_fp <= fp) {
            break;
        }
        fp = next_fp;
    }

    /* Stage 2: Stack Text Scanner fallback when FP chain yields <= 1 frame beyond PC */
    if (fp_frames <= 1U && count < max_frames) {
        if (init_frame->lr != 0U &&
            addr_is_valid_text(init_frame->lr, text_start, text_end, false)) {
            if (!thumb_check_bl || thumb_is_preceded_by_bl(init_frame->lr, text_start)) {
                count = push_unique_pc(out_pcs, count, max_frames, init_frame->lr);
            }
        }

        if (init_frame->sp >= init_frame->stack_low &&
            init_frame->sp < init_frame->stack_high) {
            scan = (init_frame->sp + (sizeof(uintptr_t) - 1U)) & ~(sizeof(uintptr_t) - 1U);
            limit = init_frame->stack_high;
            if (limit - scan > 512U) {
                limit = scan + 512U;
            }
            for (; scan + sizeof(uintptr_t) <= limit && count < max_frames;
                 scan += sizeof(uintptr_t)) {
                uintptr_t val = *(const uintptr_t *)scan;
                uintptr_t code_addr = val & ~(uintptr_t)1U;

                if (!addr_is_valid_text(code_addr, text_start, text_end, true)) {
                    continue;
                }
                if (thumb_check_bl && !thumb_is_preceded_by_bl(val, text_start)) {
                    continue;
                }
                count = push_unique_pc(out_pcs, count, max_frames, code_addr);
            }
        }
    }

    return count;
}

static void get_kernel_text_bounds(uintptr_t *text_start,
                                   uintptr_t *text_end,
                                   bool *thumb_check_bl)
{
    uintptr_t start = 0U;
    uintptr_t end = 0U;
    bool check_bl = false;

    if (kallsyms_num > 0U) {
        start = (uintptr_t)kallsyms_table[0].addr & ~(uintptr_t)1U;
        end = ((uintptr_t)kallsyms_table[kallsyms_num - 1U].addr & ~(uintptr_t)1U) + 0x1000U;
    }
#if defined(__arm__) || defined(__thumb__)
    {
        extern char _etext[];
        if (start == 0U) {
            start = 0x08000000U;
        }
        if ((uintptr_t)_etext > end) {
            end = (uintptr_t)_etext;
        }
        check_bl = true;
    }
#endif
    *text_start = start;
    *text_end = end;
    *thumb_check_bl = check_bl;
}

size_t unwind_task_frames(struct task_struct *task,
                          uintptr_t *out_pcs,
                          size_t max_frames)
{
    struct unwind_frame frame;
    uintptr_t text_start = 0U;
    uintptr_t text_end = 0U;
    bool thumb_check_bl = false;

    if (task == NULL || out_pcs == NULL || max_frames == 0U) {
        return 0U;
    }

    memset(&frame, 0, sizeof(frame));
    task_get_stack_bounds(task, &frame.stack_low, &frame.stack_high);

    if (task == current) {
#if defined(__arm__) || defined(__thumb__)
        uintptr_t cur_sp;
        uintptr_t cur_fp;
        uintptr_t cur_lr;
        __asm__ volatile ("mov %0, sp" : "=r"(cur_sp));
        __asm__ volatile ("mov %0, r7" : "=r"(cur_fp));
        __asm__ volatile ("mov %0, lr" : "=r"(cur_lr));
        frame.sp = cur_sp;
        frame.fp = cur_fp;
        frame.lr = cur_lr;
        frame.pc = (uintptr_t)unwind_task_frames;
#else
        frame.sp = (uintptr_t)&frame;
        frame.fp = (uintptr_t)__builtin_frame_address(0);
        frame.lr = (uintptr_t)__builtin_return_address(0);
        frame.pc = (uintptr_t)unwind_task_frames;
#endif
        if ((frame.sp < frame.stack_low || frame.sp >= frame.stack_high) &&
            task->sp != NULL &&
            (uintptr_t)task->sp >= frame.stack_low &&
            (uintptr_t)task->sp < frame.stack_high) {
            frame.sp = (uintptr_t)task->sp;
            if (task->wchan_addr != 0U) {
                frame.pc = task->wchan_addr;
            }
        }
    } else if (task->sp != NULL) {
#if defined(__arm__) || defined(__thumb__)
        const uintptr_t *ctx = (const uintptr_t *)task->sp;
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
        uint32_t fpu_flag = 0U;
        if ((uintptr_t)ctx >= frame.stack_low &&
            (uintptr_t)(ctx + 1) <= frame.stack_high) {
            fpu_flag = (uint32_t)*ctx++;
        }
#endif
        if ((uintptr_t)ctx >= frame.stack_low &&
            (uintptr_t)(ctx + 8) <= frame.stack_high) {
            frame.fp = ctx[3];
            ctx += 8;
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
            if (fpu_flag != 0U && (uintptr_t)(ctx + 16) <= frame.stack_high) {
                ctx += 16;
            }
#endif
            if ((uintptr_t)(ctx + 8) <= frame.stack_high) {
                frame.lr = ctx[5];
                frame.pc = ctx[6];
                frame.sp = (uintptr_t)(ctx + 8);
            } else {
                frame.sp = (uintptr_t)ctx;
                frame.pc = task->wchan_addr;
            }
        } else {
            frame.sp = (uintptr_t)task->sp;
            frame.pc = task->wchan_addr;
        }
#else
        frame.sp = (uintptr_t)task->sp;
        if (frame.sp < frame.stack_low || frame.sp > frame.stack_high) {
            frame.sp = frame.stack_low;
        }
        frame.pc = task->wchan_addr;
        frame.lr = 0U;
        frame.fp = 0U;
#endif
    }

    get_kernel_text_bounds(&text_start, &text_end, &thumb_check_bl);
    return unwind_frames_from_state(&frame,
                                    text_start,
                                    text_end,
                                    thumb_check_bl,
                                    out_pcs,
                                    max_frames);
}

static void dump_one_frame(unwind_print_fn_t out_fn, unsigned int idx, uintptr_t pc)
{
    const char *name;
    unsigned long symsize = 0UL;
    unsigned long offset = 0UL;

    name = kallsyms_lookup((unsigned long)pc, &symsize, &offset, NULL, NULL);
    if (name != NULL) {
        if (symsize > 0UL) {
            out_fn("  #%02u [<0x%08lx>] %s+0x%lx/0x%lx\n",
                   idx, (unsigned long)pc, name, offset, symsize);
        } else {
            out_fn("  #%02u [<0x%08lx>] %s+0x%lx\n",
                   idx, (unsigned long)pc, name, offset);
        }
    } else {
        out_fn("  #%02u [<0x%08lx>] 0x%08lx\n",
               idx, (unsigned long)pc, (unsigned long)pc);
    }
}

void dump_task_stack(struct task_struct *task, unwind_print_fn_t print_fn)
{
    uintptr_t pcs[DUMP_LOCAL_MAX_FRAMES];
    unwind_print_fn_t out_fn = (print_fn != NULL) ? print_fn : default_dump_print;
    size_t count;
    size_t i;

    if (task == NULL) {
        return;
    }

    out_fn("Call Trace (task=%s, pid=%u, state=%s):\n",
           task->name ? task->name : "-",
           (unsigned int)task->pid,
           task_state_short_str(task->status));

    count = unwind_task_frames(task, pcs, DUMP_LOCAL_MAX_FRAMES);
    if (count == 0U) {
        out_fn("  <no frames>\n");
        return;
    }
    for (i = 0U; i < count; i++) {
        dump_one_frame(out_fn, (unsigned int)i, pcs[i]);
    }
}

void dump_trap_backtrace(uintptr_t sp,
                         uintptr_t pc,
                         uintptr_t lr,
                         uintptr_t fp,
                         uintptr_t stack_low,
                         uintptr_t stack_high,
                         unwind_print_fn_t print_fn)
{
    uintptr_t pcs[DUMP_LOCAL_MAX_FRAMES];
    struct unwind_frame frame;
    unwind_print_fn_t out_fn = (print_fn != NULL) ? print_fn : default_dump_print;
    uintptr_t text_start = 0U;
    uintptr_t text_end = 0U;
    bool thumb_check_bl = false;
    size_t count;
    size_t i;

    frame.pc = pc;
    frame.lr = lr;
    frame.sp = sp;
    frame.fp = fp;
    frame.stack_low = stack_low;
    frame.stack_high = stack_high;

    if (frame.stack_low == 0U || frame.stack_high <= frame.stack_low) {
        if (current != NULL) {
            task_get_stack_bounds(current, &frame.stack_low, &frame.stack_high);
        }
        if (sp < frame.stack_low || sp >= frame.stack_high) {
            frame.stack_low = sp & ~(uintptr_t)(sizeof(uintptr_t) - 1U);
            frame.stack_high = frame.stack_low + 512U;
        }
    }

    get_kernel_text_bounds(&text_start, &text_end, &thumb_check_bl);
    out_fn("Call Trace (trap):\n");
    count = unwind_frames_from_state(&frame,
                                     text_start,
                                     text_end,
                                     thumb_check_bl,
                                     pcs,
                                     DUMP_LOCAL_MAX_FRAMES);
    if (count == 0U) {
        out_fn("  <no frames>\n");
        return;
    }
    for (i = 0U; i < count; i++) {
        dump_one_frame(out_fn, (unsigned int)i, pcs[i]);
    }
}

void dump_sched_rq(int (*print_fn)(const char *fmt, ...))
{
    unwind_print_fn_t out_fn = (print_fn != NULL) ? print_fn : default_dump_print;
    uint32_t cpu;

    for (cpu = 0U; cpu < (uint32_t)CONFIG_NR_CPUS; cpu++) {
        struct sched_rq *rq = sched_rq_of(cpu);
        uint32_t util_permille;
        if (rq == NULL) {
            continue;
        }
        util_permille = ((uint32_t)rq->cpu_util_avg * 1000U) >> SCHED_CAPACITY_SHIFT;
        out_fn("CPU%u: nr_run=%u util=%u.%u%% csw=%llu curr=%s rt_nr=%u fair_nr=%u min_vruntime=%llu\n",
               (unsigned int)rq->cpu_id,
               (unsigned int)rq->nr_running,
               (unsigned int)(util_permille / 10U),
               (unsigned int)(util_permille % 10U),
               (unsigned long long)rq->nr_switches,
               (rq->curr && rq->curr->name) ? rq->curr->name : "none",
               (unsigned int)rq->rt.nr_running,
               (unsigned int)rq->fair.nr_running,
               (unsigned long long)rq->fair.min_vruntime);
    }
}

struct task_summary_counts {
    uint16_t total;
    uint16_t run;
    uint16_t sleep;
    uint16_t stop;
    uint16_t zombie;
};

static int count_tasks_cb(struct task_struct *task, void *arg)
{
    struct task_summary_counts *c = arg;

    c->total++;
    switch (task->status) {
    case TASK_RUNNING:
    case TASK_READY:
        c->run++;
        break;
    case TASK_SLEEPING:
    case TASK_BLOCKED:
        c->sleep++;
        break;
    case TASK_SUSPENDED:
        c->stop++;
        break;
    case TASK_ZOMBIE:
        c->zombie++;
        break;
    default:
        break;
    }
    return 0;
}

static const char *resolve_task_wchan(struct task_struct *task)
{
    const char *sym;

    if (task == NULL) {
        return "-";
    }
    if (task->status != TASK_SLEEPING && task->status != TASK_BLOCKED) {
        return "-";
    }
    if (task->wchan_name != NULL && task->wchan_name[0] != '\0') {
        return task->wchan_name;
    }
    if (task->wchan_addr != 0U) {
        sym = kallsyms_lookup((unsigned long)task->wchan_addr, NULL, NULL, NULL, NULL);
        if (sym != NULL) {
            return sym;
        }
        return "blocked";
    }
    return "-";
}

struct dump_row_ctx {
    unwind_print_fn_t out_fn;
    enum task_ps_mode mode;
};

static int dump_task_row_cb(struct task_struct *task, void *arg)
{
    struct dump_row_ctx *ctx = arg;
    u32 usage = task_get_cpu_usage(task);
    uint64_t total_cs = task->se.sum_exec_us / 10000ULL;
    uint32_t cs = (uint32_t)(total_cs % 100ULL);
    uint32_t total_sec = (uint32_t)(total_cs / 100ULL);
    size_t max_used = task_stack_max_used(task);
    char health = task_stack_is_healthy(task) ? ' ' : '!';
    const char *wchan = resolve_task_wchan(task);

    if (ctx->mode == TASK_PS_LONG) {
        ctx->out_fn("%4u %4u %3u %-3s %4u %4u %4d %-4s %3u.%u %02u:%02u.%02u %12llu %5u/%-5u%c %6u/%-6u 0x%02x %-16s %s\n",
                    (unsigned int)task->pid,
                    (unsigned int)task->ppid,
                    (unsigned int)task->cpu,
                    task_policy_short_str(task->policy),
                    (unsigned int)task->current_priority,
                    (unsigned int)task->init_priority,
                    (int)task->nice,
                    task_state_short_str(task->status),
                    (unsigned int)(usage / 100U),
                    (unsigned int)((usage % 100U) / 10U),
                    (unsigned int)(total_sec / 60U),
                    (unsigned int)(total_sec % 60U),
                    (unsigned int)cs,
                    (unsigned long long)task->se.vruntime,
                    (unsigned int)max_used,
                    (unsigned int)task->stack_size,
                    health,
                    (unsigned int)task->se.nvcsw,
                    (unsigned int)task->se.nivcsw,
                    (unsigned int)task->cpus_allowed,
                    wchan,
                    task->name ? task->name : "-");
    } else {
        ctx->out_fn("%4u %4u %3u %-3s %4u %-4s %3u.%u %02u:%02u.%02u %5u/%-5u%c %5u %-16s %s\n",
                    (unsigned int)task->pid,
                    (unsigned int)task->ppid,
                    (unsigned int)task->cpu,
                    task_policy_short_str(task->policy),
                    (unsigned int)task->current_priority,
                    task_state_short_str(task->status),
                    (unsigned int)(usage / 100U),
                    (unsigned int)((usage % 100U) / 10U),
                    (unsigned int)(total_sec / 60U),
                    (unsigned int)(total_sec % 60U),
                    (unsigned int)cs,
                    (unsigned int)max_used,
                    (unsigned int)task->stack_size,
                    health,
                    (unsigned int)(task->se.nvcsw + task->se.nivcsw),
                    wchan,
                    task->name ? task->name : "-");
    }
    return 0;
}

static int dump_task_stack_cb(struct task_struct *task, void *arg)
{
    unwind_print_fn_t out_fn = (unwind_print_fn_t)arg;
    dump_task_stack(task, out_fn);
    return 0;
}

void dump_all_task_ex(int (*print_fn)(const char *fmt, ...), enum task_ps_mode mode)
{
    unwind_print_fn_t out_fn = (print_fn != NULL) ? print_fn : default_dump_print;
    struct task_summary_counts counts = { 0U, 0U, 0U, 0U, 0U };
    struct dump_row_ctx row_ctx;
    uint32_t loads[3] = { 0U, 0U, 0U };
    u32 cpu_usage;

    (void)task_foreach(count_tasks_cb, &counts);
    cpu_usage = get_cpu_usage();
    sched_get_loadavg(loads, NULL, NULL);

    out_fn("Tasks: %u total, %u run, %u sleep, %u stop, %u zombie | CPU: %u.%u%% | Load: %u.%02u, %u.%02u, %u.%02u\n",
           (unsigned int)counts.total,
           (unsigned int)counts.run,
           (unsigned int)counts.sleep,
           (unsigned int)counts.stop,
           (unsigned int)counts.zombie,
           (unsigned int)(cpu_usage / 100U),
           (unsigned int)((cpu_usage % 100U) / 10U),
           (unsigned int)SCHED_LOAD_INT(loads[0]),
           (unsigned int)SCHED_LOAD_FRAC(loads[0]),
           (unsigned int)SCHED_LOAD_INT(loads[1]),
           (unsigned int)SCHED_LOAD_FRAC(loads[1]),
           (unsigned int)SCHED_LOAD_INT(loads[2]),
           (unsigned int)SCHED_LOAD_FRAC(loads[2]));

    if (mode == TASK_PS_LONG) {
        out_fn(" PID PPID CPU POL PRIO BASE NICE STAT  %%CPU     TIME      VRUNTIME STK_USED/SIZE   NVCSW/NIVCSW  AFF WCHAN            NAME\n");
    } else {
        out_fn(" PID PPID CPU POL PRIO STAT  %%CPU     TIME   STK_USED/SIZE   CSW  WCHAN            NAME\n");
    }

    row_ctx.out_fn = out_fn;
    row_ctx.mode = mode;
    (void)task_foreach(dump_task_row_cb, &row_ctx);

    if (mode == TASK_PS_STACK) {
        (void)task_foreach(dump_task_stack_cb, (void *)out_fn);
    }
}

void dump_all_task_to(int (*print_fn)(const char *fmt, ...))
{
    dump_all_task_ex(print_fn, TASK_PS_NORMAL);
}

void dump_all_task(void)
{
    dump_all_task_ex(NULL, TASK_PS_NORMAL);
}

int dump_task_detail(pid_t pid, int (*print_fn)(const char *fmt, ...), bool include_stack)
{
    unwind_print_fn_t out_fn = (print_fn != NULL) ? print_fn : default_dump_print;
    struct task_struct *task = task_find_by_pid(pid);
    uintptr_t stack_low = 0U;
    uintptr_t stack_high = 0U;
    u32 usage;

    if (task == NULL) {
        return -ESRCH;
    }

    task_get_stack_bounds(task, &stack_low, &stack_high);
    usage = task_get_cpu_usage(task);

    out_fn("Task: %s (PID=%u, PPID=%u, State=%s, Policy=%s)\n",
           task->name ? task->name : "-",
           (unsigned int)task->pid,
           (unsigned int)task->ppid,
           task_state_short_str(task->status),
           task_policy_short_str(task->policy));
    out_fn("CPU: %u (on_cpu=%u, affinity=0x%02x) | Prio: cur=%u base=%u boost=%u nice=%d\n",
           (unsigned int)task->cpu,
           (unsigned int)task->on_cpu,
           (unsigned int)task->cpus_allowed,
           (unsigned int)task->current_priority,
           (unsigned int)task->init_priority,
           (unsigned int)task->pi_boost_prio,
           (int)task->nice);
    out_fn("Sched: util=%u.%u%% exec_us=%llu vruntime=%llu weight=%u csw=%u/%u\n",
           (unsigned int)(usage / 100U),
           (unsigned int)((usage % 100U) / 10U),
           (unsigned long long)task->se.sum_exec_us,
           (unsigned long long)task->se.vruntime,
           (unsigned int)task->se.weight,
           (unsigned int)task->se.nvcsw,
           (unsigned int)task->se.nivcsw);
    out_fn("Stack: low=0x%08lx high=0x%08lx sp=0x%08lx used=%u/%u healthy=%s\n",
           (unsigned long)stack_low,
           (unsigned long)stack_high,
           (unsigned long)(uintptr_t)task->sp,
           (unsigned int)task_stack_max_used(task),
           (unsigned int)task->stack_size,
           task_stack_is_healthy(task) ? "yes" : "OVERFLOW");
    out_fn("WCHAN: %s (0x%08lx)\n",
           resolve_task_wchan(task),
           (unsigned long)task->wchan_addr);

    if (include_stack) {
        dump_task_stack(task, out_fn);
    }
    return 0;
}
