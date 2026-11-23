/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS Agent Cell 微容器安全边界：
 * 五维硬预算隔离、EL0 只读 Telemetry 共享页与 64B 内核强制环形审计区。
 */

#ifndef __NOS_CELL_H__
#define __NOS_CELL_H__

#include <uapi/nos/types.h>
#include <uapi/nos/ai.h>
#include <kernel/spinlock.h>
#include <nos/object.h>
#include <nos/cap.h>
#include <nos/mm.h>

#define NOS_CELL_AUDIT_RING_CAPACITY    64U
#define NOS_CELL_MAX_TRACKED_VMOS       16U

struct task_struct;
struct nos_notification;
struct nos_vmo;

struct nos_cell_vmo_binding {
    bool            active;
    struct nos_vmo *vmo;
    uintptr_t       vaddr;
    size_t          offset;
    size_t          len;
    uint32_t        prot_flags;
};

struct nos_cell_snapshot_item {
    struct nos_vmo *live_vmo;
    struct nos_vmo *frozen_vmo;
    uintptr_t       vaddr;
    size_t          offset;
    size_t          len;
    uint32_t        prot_flags;
};

struct nos_cell_snapshot {
    struct nos_object             header; /* 必须置于首字段，供 Capability Pinned Lookup */
    struct nos_cell              *owner_cell;
    struct nos_cap_table_snapshot *cap_snap;
    uint32_t                      item_count;
    uint16_t                      saved_state;
    uint16_t                      reserved;
    uint64_t                      saved_binding_mask;
    uint64_t                      saved_cpu_ns_used;
    uint64_t                      saved_mem_bytes_used;
    uint64_t                      saved_tool_calls_used;
    uint64_t                      saved_accel_ops_used;
    struct nos_cell_snapshot_item items[NOS_CELL_MAX_TRACKED_VMOS];
};

struct nos_cell {
    struct nos_object        header;          /* 必须置于首字段，用于 Capability Pinned Lookup */
    uint32_t                 cell_id;         /* 全局递增 Cell 唯一 ID */
    uint16_t                 state;           /* NOS_CELL_STATE_* */
    uint16_t                 reserved;
    uint32_t                 iommu_sid;       /* 最近绑定的加速器 SMMU StreamID */
    bool                     has_iommu_sid;
    spinlock_t               lock;            /* 保护预算计数、Telemetry 同步与审计环 */
    struct nos_cell         *parent;          /* 父 Cell（用于级联预算约束与子数量归还） */
    struct nos_cap_table     cap_table;       /* Cell 专属能力空间根 CNode */
    struct nos_vm_space      vm_space;        /* Cell 专属硬件地址空间（TTBR0 + ASID） */
    struct nos_cell_budget   budget;          /* 当前生效的五维硬预算配额上限 */
    uint64_t                 cpu_ns_used;     /* 已消耗 CPU 时间（纳秒） */
    uint64_t                 mem_bytes_used;  /* 当前记账内存占用（字节） */
    uint64_t                 tool_calls_used; /* 已消耗工具调用次数 */
    uint64_t                 accel_ops_used;  /* 已消耗加速器提交次数 */
    uint32_t                 child_cells_used;/* 当前存活子 Cell 数量 */
    bool                     parent_accounted;/* 销毁防重复扣减 parent->child_cells_used */
    uintptr_t                telemetry_kvaddr;/* 4KB EL0 只读 Telemetry 共享页内核虚址 */
    uintptr_t                telemetry_paddr; /* Telemetry 物理页地址 */
    struct nos_notification *budget_notif;    /* 预算触发熔断时的异步通知对象 */
    struct nos_audit_record  audit_ring[NOS_CELL_AUDIT_RING_CAPACITY];
    uint32_t                 audit_head;      /* 环形写入游标（0..63） */
    uint32_t                 audit_count;     /* 当前已缓存审计记录条数（<= 64） */
    uint64_t                 audit_seq;       /* 累计写入审计记录单调序号 */
    struct nos_cell_vmo_binding vmo_bindings[NOS_CELL_MAX_TRACKED_VMOS];
};

void nos_cell_subsystem_init(void);

struct nos_cell *nos_cell_create(struct nos_cell *parent,
                                 const struct nos_cell_budget *budget);
struct nos_cell *nos_cell_fork(struct nos_cell *parent,
                               const struct nos_cell_budget *budget);
int nos_cell_destroy(struct nos_cell *cell);
void nos_cell_notify_event(struct nos_cell *cell, uint64_t evt_bits);
int nos_cell_bind_iommu_sid(struct nos_cell *cell, uint32_t sid);

int nos_cell_set_budget(struct nos_cell *cell, const struct nos_cell_budget *budget);
int nos_cell_get_budget(struct nos_cell *cell, struct nos_cell_budget *out_budget);
int nos_cell_bind_budget_notif(struct nos_cell *cell, struct nos_notification *notif);

int nos_cell_map_telemetry(struct nos_cell *cell, uintptr_t user_vaddr);
const struct nos_cell_telemetry *nos_cell_telemetry_view(const struct nos_cell *cell);

int nos_cell_charge_cpu(struct nos_cell *cell, uint64_t delta_ns);
int nos_cell_charge_mem(struct nos_cell *cell, uint64_t bytes);
void nos_cell_uncharge_mem(struct nos_cell *cell, uint64_t bytes);
int nos_cell_charge_tool_call(struct nos_cell *cell, uint64_t schema_id, uint32_t badge);
int nos_cell_charge_accel_op(struct nos_cell *cell, uint64_t schema_id, uint32_t badge);

void nos_cell_record_audit(struct nos_cell *cell,
                           uint16_t event_type,
                           uint64_t schema_id,
                           uint32_t badge,
                           int32_t status,
                           uint64_t arg0,
                           uint64_t arg1);
int nos_cell_read_audit(struct nos_cell *cell,
                        struct nos_audit_record *out_buf,
                        uint32_t max_records);

struct nos_vm_space *nos_cell_vm_space(struct nos_cell *cell);
struct nos_cap_table *nos_cell_cap_table(struct nos_cell *cell);

int nos_cell_attach_task(struct task_struct *task, struct nos_cell *cell);
void nos_cell_detach_task(struct task_struct *task);

int nos_cell_track_vmo_map(struct nos_cell *cell,
                           struct nos_vmo *vmo,
                           uintptr_t vaddr,
                           size_t offset,
                           size_t len,
                           uint32_t prot_flags);
void nos_cell_untrack_vmo_map(struct nos_cell *cell, uintptr_t vaddr, size_t len);

struct nos_cell_snapshot *nos_cell_snapshot(struct nos_cell *cell);
int nos_cell_rollback(struct nos_cell *cell, struct nos_cell_snapshot *snap);
int nos_cell_handle_cow_fault(struct nos_cell *cell, uintptr_t fault_va);
int nos_cell_handle_page_fault(struct nos_cell *cell,
                               uintptr_t fault_va,
                               bool is_write,
                               uint32_t dfsc);
int nos_cell_snapshot_destroy(struct nos_cell_snapshot *snap);

#endif /* __NOS_CELL_H__ */
