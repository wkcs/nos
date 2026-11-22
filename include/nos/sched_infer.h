/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS AI 推理感知 SLO 调度器接口 (EDF + CBS + Deadline Inheritance + EAS + Gang)
 */

#ifndef __NOS_SCHED_INFER_H__
#define __NOS_SCHED_INFER_H__

#include <uapi/nos/types.h>
#include <uapi/nos/ai.h>

struct task_struct;

#define NOS_GANG_MAX_GROUPS     16U
#define NOS_GANG_MAX_MEMBERS    8U

int sched_infer_set_param(struct task_struct *task,
                          const struct nos_infer_sched_param *param);

int sched_infer_boost_deadline(struct task_struct *target,
                               uint64_t inherited_abs_deadline_ns);
void sched_infer_restore_deadline(struct task_struct *target);

int sched_infer_gang_join(struct task_struct *task, uint32_t gang_id);
void sched_infer_gang_leave(struct task_struct *task);
int sched_infer_gang_wakeup(uint32_t gang_id);

int sched_infer_query_topo(struct nos_cpu_topo_info *out_info);
int arch_sched_query_topo(struct nos_cpu_topo_info *out_info);

#endif /* __NOS_SCHED_INFER_H__ */
