/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS AI 时代原生内核对象 UAPI/ABI 契约（Agent Cell / VMO / RingIPC / 推理调度）。
 * 约束：所有结构体尺寸跨配置与跨架构严格锁定，禁止通过 #ifdef 改变布局。
 */

#ifndef __NOS_UAPI_AI_H__
#define __NOS_UAPI_AI_H__

#include <uapi/nos/types.h>

/* ---------------- Agent Cell 多维硬预算与操作原语 ---------------- */

#define NOS_CELL_OP_CREATE      1U
#define NOS_CELL_OP_DESTROY     2U
#define NOS_CELL_OP_SET_BUDGET  3U
#define NOS_CELL_OP_GET_BUDGET  4U
#define NOS_CELL_OP_FORK        5U
#define NOS_CELL_OP_SNAPSHOT    6U
#define NOS_CELL_OP_ROLLBACK    7U
#define NOS_CELL_OP_READ_AUDIT  8U

/* Agent Cell 只读 Telemetry 共享页契约 */
#define NOS_CELL_TELEMETRY_MAGIC    0x4E43454CU /* "NCEL" */

#define NOS_CELL_STATE_ACTIVE       1U
#define NOS_CELL_STATE_BUDGET_HIT   2U
#define NOS_CELL_STATE_SUSPENDED    3U
#define NOS_CELL_STATE_DESTROYED    4U

/* Cell 审计事件类型 */
#define NOS_AUDIT_CAP_DERIVE    1U
#define NOS_AUDIT_CAP_REVOKE    2U
#define NOS_AUDIT_TOOL_CALL     3U
#define NOS_AUDIT_ACCEL_JOB     4U
#define NOS_AUDIT_BUDGET_HIT    5U
#define NOS_AUDIT_FAULT         6U

struct nos_cell_budget {
    uint64_t cpu_ns_limit;      /* 最大可用 CPU 时间配额（纳秒，0 表示无限制） */
    uint64_t mem_bytes_limit;   /* 最大可用物理/虚拟内存配额（字节） */
    uint64_t tool_calls_limit;  /* RingIPC 工具调用总次数配额 */
    uint64_t accel_ops_limit;   /* NPU/GPU 加速器提交次数配额 */
    uint32_t max_child_cells;   /* 最大允许派生的子 Cell 数量 */
    uint32_t flags;             /* 控制标志位 */
};

/* 映射至 EL0 只读 Telemetry 页首部的实时观测结构体（严格 96 字节） */
struct nos_cell_telemetry {
    uint32_t magic;                 /* NOS_CELL_TELEMETRY_MAGIC */
    uint16_t version;               /* 结构体版本（当前为 1） */
    uint16_t state;                 /* NOS_CELL_STATE_* */
    uint32_t cell_id;               /* Cell 全局唯一标识 */
    uint32_t child_cells_used;      /* 当前存活子 Cell 数量 */
    uint64_t cpu_ns_used;           /* 已消耗 CPU 时间（纳秒） */
    uint64_t mem_bytes_used;        /* 当前记账内存占用（字节） */
    uint64_t tool_calls_used;       /* 已消耗工具调用次数 */
    uint64_t accel_ops_used;        /* 已消耗加速器操作次数 */
    uint64_t audit_seq;             /* 已记录的审计事件累计序列号 */
    struct nos_cell_budget limit;   /* 当前五维预算配额上限 */
};

/* 内核态强制写入的 64 字节定长审计记录（1 条 Cache Line） */
struct nos_audit_record {
    uint64_t timestamp_ns;      /* 单调纳秒时间戳 */
    uint64_t schema_id;         /* 工具调用 Schema 或对象标识 */
    uint32_t cell_id;           /* 触发审计的 Cell ID */
    uint16_t event_type;        /* NOS_AUDIT_* */
    uint16_t flags;             /* 结果/拒绝标志 */
    int32_t  status;            /* 执行结果或负 errno */
    uint32_t cap_badge;         /* 不可伪造来源令牌 */
    uint64_t arg0;              /* 关键参数或越界地址 */
    uint64_t arg1;              /* 附加长度或配额余量 */
    uint64_t reserved[2];       /* 补齐至 64 字节 */
};

/* ---------------- VMO / TensorObj 与 OS 级 KV-Cache ---------------- */

enum nos_vmo_kind {
    NOS_VMO_ANON       = 0,     /* 常规匿名工作内存 */
    NOS_VMO_WEIGHT     = 1,     /* 只读共享模型权重（大页优先 + 可密封） */
    NOS_VMO_KVCACHE    = 2,     /* 操作系统级 PagedAttention KV-Cache 对象 */
    NOS_VMO_DMA_TENSOR = 3,     /* 异构加速器零拷贝 DMA 张量对象 */
};

#define NOS_VMO_FLAG_HUGEPAGE_2M    (1U << 0)
#define NOS_VMO_FLAG_CONTIG_64K     (1U << 1)
#define NOS_VMO_FLAG_RECOMPUTABLE   (1U << 2)   /* 允许在内存紧缺时协同驱逐 */
#define NOS_VMO_FLAG_IO_COHERENT    (1U << 3)

#define NOS_VMO_OP_SEAL             1U          /* 永久封印为只读不可变权重 */
#define NOS_VMO_OP_CLONE_COW        2U          /* 写时复制克隆 */
#define NOS_VMO_OP_CLONE_KV_PREFIX  3U          /* 跨 Cell 共享公共 KV 前缀块 */
#define NOS_VMO_OP_BIND_EVICT_NOTIF 4U          /* 绑定 KV 块驱逐通知通道 */
#define NOS_VMO_OP_SYNC_DMA         5U          /* 显式维护 DMA Cache 一致性 */
#define NOS_VMO_OP_MAP              6U          /* 将 VMO 映射至目标地址空间 */
#define NOS_VMO_OP_UNMAP            7U          /* 从目标地址空间解除映射 */

struct nos_vmo_create_args {
    uint64_t size_bytes;        /* VMO 虚拟大小（页对齐） */
    uint32_t kind;              /* enum nos_vmo_kind */
    uint32_t flags;             /* NOS_VMO_FLAG_* */
    uint32_t block_bytes;       /* KV 块粒度（默认 65536） */
    uint32_t reserved;
    uint64_t reserved_ext;
};

/* ---------------- RingIPC 异步提交/完成环 ---------------- */

#define NOS_RING_OP_NOP             0U
#define NOS_RING_OP_TOOL_CALL       1U
#define NOS_RING_OP_STREAM_CHUNK    2U
#define NOS_RING_OP_TOKEN_EMIT      3U
#define NOS_RING_OP_VMO_XFER        4U
#define NOS_RING_OP_ACCEL_JOB       5U
#define NOS_RING_OP_CANCEL          6U

#define NOS_RING_FLAG_ZERO_COPY_RO  (1U << 0)
#define NOS_RING_FLAG_MOVE_SLICE    (1U << 1)
#define NOS_RING_FLAG_NEEDS_AUDIT   (1U << 2)
#define NOS_RING_FLAG_DEADLINE      (1U << 3)

/* 提交队列条目：严格 64 字节（单条 ARM64 Cache Line） */
struct nos_ring_sqe {
    uint16_t opcode;            /* NOS_RING_OP_* */
    uint16_t flags;             /* NOS_RING_FLAG_* */
    uint32_t target_cap;        /* 目标工具服务 / NPU / 子 Agent 句柄 */
    uint64_t req_id;            /* 并发乱序关联 ID */
    uint64_t schema_id;         /* 强类型工具契约 Schema 哈希 */
    uint32_t vmo_cap;           /* 零拷贝传递的 VMO 句柄 */
    uint32_t vmo_len;           /* 载荷切片长度 */
    uint64_t vmo_offset;        /* 载荷切片偏移 */
    uint64_t deadline_ns;       /* 端到端截止期（触发 Deadline Inheritance） */
    uint64_t inline_arg[2];     /* 16 字节短参数内联快路 */
};

/* 完成队列条目：严格 32 字节 */
struct nos_ring_cqe {
    uint64_t req_id;            /* 对应 SQE 的 req_id */
    int32_t  status;            /* 0 或 -errno */
    uint32_t result_vmo_cap;    /* 可选返回的结果 VMO 句柄 */
    uint32_t result_len;        /* 结果字节长度 */
    uint32_t flags;             /* 完成标志 */
    uint64_t elapsed_ns;        /* 服务端实际耗时（纳秒） */
};

/* ---------------- 推理感知调度与异构加速 ---------------- */

enum nos_infer_role {
    NOS_INFER_ROLE_NONE        = 0,
    NOS_INFER_ROLE_DECODE      = 1, /* TPOT 尾延迟敏感（优先带宽隔离核） */
    NOS_INFER_ROLE_PREFILL     = 2, /* 算力敏感（优先 SVE/SME/I8MM 大核） */
    NOS_INFER_ROLE_AGENT_CTRL  = 3, /* Agent 控制流（优先小核低功耗） */
    NOS_INFER_ROLE_TOOL_WORKER = 4, /* 工具代理（支持截止期继承） */
};

#define NOS_SCHED_OP_SET_INFER      1U
#define NOS_SCHED_OP_GANG_JOIN      2U
#define NOS_SCHED_OP_GANG_LEAVE     3U
#define NOS_SCHED_OP_QUERY_TOPO     4U

struct nos_infer_sched_param {
    uint32_t role;              /* enum nos_infer_role */
    uint32_t gang_id;           /* 协同 Gang 组编号（0 表示不加入） */
    uint64_t runtime_ns;        /* CBS 周期预算运行时间 */
    uint64_t deadline_ns;       /* 相对 SLO 截止期 */
    uint64_t period_ns;         /* 周期时长 */
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct nos_cell_budget) == 40, "nos_cell_budget ABI 尺寸必须为 40 字节");
_Static_assert(sizeof(struct nos_cell_telemetry) == 96, "nos_cell_telemetry ABI 尺寸必须为 96 字节");
_Static_assert(sizeof(struct nos_audit_record) == 64, "nos_audit_record ABI 尺寸必须为 64 字节");
_Static_assert(sizeof(struct nos_vmo_create_args) == 32, "nos_vmo_create_args ABI 尺寸必须为 32 字节");
_Static_assert(sizeof(struct nos_ring_sqe) == 64, "nos_ring_sqe ABI 尺寸必须为 64 字节");
_Static_assert(sizeof(struct nos_ring_cqe) == 32, "nos_ring_cqe ABI 尺寸必须为 32 字节");
_Static_assert(sizeof(struct nos_infer_sched_param) == 32, "nos_infer_sched_param ABI 尺寸必须为 32 字节");
#endif

#endif /* __NOS_UAPI_AI_H__ */
