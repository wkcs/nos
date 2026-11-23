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
#define NOS_AUDIT_CELL_SNAPSHOT 7U
#define NOS_AUDIT_CELL_ROLLBACK 8U
#define NOS_AUDIT_ACCEL_SUBMIT  9U
#define NOS_AUDIT_IOMMU_FAULT   10U

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

#define NOS_RING_MAGIC              0x4E524E47U /* "NRNG" */
#define NOS_RING_DEFAULT_ENTRIES    32U
#define NOS_RING_MAX_SCHEMAS        16U

#define NOS_RING_CREATE_SCHEMA_FILTER (1U << 0)

#define NOS_RING_ENTER_SUBMIT       (1U << 0)
#define NOS_RING_ENTER_WAIT_CQ      (1U << 1)
#define NOS_RING_ENTER_COMPLETE     (1U << 2)

/* RingIPC 共享页头部：严格 128 字节（两条 Cache Line 消除 SQ/CQ 伪共享） */
struct nos_ring_header {
    uint32_t magic;             /* NOS_RING_MAGIC */
    uint16_t version;           /* 结构体版本（当前为 1） */
    uint16_t flags;             /* NOS_RING_CREATE_* */
    uint32_t sq_entries;        /* SQ 槽位数（2 的幂，默认 32） */
    uint32_t cq_entries;        /* CQ 槽位数（2 的幂，默认 32） */
    uint32_t sq_head;           /* 内核消费者头指针 */
    uint32_t sq_tail;           /* EL0 生产者尾指针（release） */
    uint32_t sq_dropped;        /* 格式错误丢弃计数 */
    uint32_t sq_offset;         /* SQ 数组相对页首字节偏移（128） */
    uint64_t reserved0[4];

    uint32_t cq_head;           /* EL0 消费者头指针 */
    uint32_t cq_tail;           /* 内核/服务端生产者尾指针（release） */
    uint32_t cq_overflow;       /* CQ 溢出计数 */
    uint32_t cq_offset;         /* CQ 数组相对页首字节偏移 */
    uint64_t total_submitted;   /* 累计提交 SQE 总数 */
    uint64_t total_completed;   /* 累计完成 CQE 总数 */
    uint64_t reserved1[4];
};

/* 创建 RingIPC 实例的系统调用参数（严格 32 字节） */
struct nos_ring_create_args {
    uint32_t sq_entries;        /* 请求 SQ 容量（0 取默认 32） */
    uint32_t cq_entries;        /* 请求 CQ 容量（0 取默认 32） */
    uint32_t flags;             /* NOS_RING_CREATE_* */
    uint32_t schema_count;      /* 白名单 Schema 数量（<= NOS_RING_MAX_SCHEMAS） */
    uint64_t schema_list_ptr;   /* 用户态 uint64_t schema_ids[] 数组指针 */
    uint64_t map_user_vaddr;    /* 可选立即将共享环页映射至此用户虚拟地址 */
};

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

/* 异构 AI 算力指令集标志（用于 NOS_SCHED_OP_QUERY_TOPO） */
#define NOS_AI_ISA_DOTPROD          (1U << 0)
#define NOS_AI_ISA_FP16             (1U << 1)
#define NOS_AI_ISA_BF16             (1U << 2)
#define NOS_AI_ISA_I8MM             (1U << 3)
#define NOS_AI_ISA_SVE              (1U << 4)
#define NOS_AI_ISA_SME              (1U << 5)

/* 异构核拓扑与 AI 加速指令查询信息（严格 32 字节） */
struct nos_cpu_topo_info {
    uint32_t nr_cpus;           /* 已注册核数 */
    uint32_t online_mask;       /* 在线核位图 */
    uint32_t big_cpu_mask;      /* 高算力大核位图 */
    uint32_t little_cpu_mask;   /* 高能效小核位图 */
    uint32_t ai_isa_flags;      /* NOS_AI_ISA_* */
    uint32_t active_gangs;      /* 当前活跃 Gang 协作组数 */
    uint64_t reserved;
};

/* ---------------- EL1 混合内核加速器与异构算子契约 ---------------- */

#define NOS_ACCEL_OP_QUERY_CAPS     1U
#define NOS_ACCEL_OP_BIND_VMO       2U
#define NOS_ACCEL_OP_SUBMIT         3U
#define NOS_ACCEL_OP_WAIT           4U
#define NOS_ACCEL_OP_CANCEL         5U
#define NOS_ACCEL_OP_RESET_CTX      6U

#define NOS_ACCEL_KERNEL_NOP            0U
#define NOS_ACCEL_KERNEL_MATMUL_I8      1U
#define NOS_ACCEL_KERNEL_GEMV_I32       2U
#define NOS_ACCEL_KERNEL_RELU_I32       3U
#define NOS_ACCEL_KERNEL_CUSTOM_BLOB    4U

#define NOS_ACCEL_TYPE_VIRTIO       1U
#define NOS_ACCEL_TYPE_RKNPU        2U

#define NOS_ACCEL_CAP_SVA           (1U << 0)
#define NOS_ACCEL_CAP_PASID         (1U << 1)
#define NOS_ACCEL_CAP_ASYNC_QUEUE   (1U << 2)
#define NOS_ACCEL_CAP_INT8_MATMUL   (1U << 3)
#define NOS_ACCEL_CAP_INT32_GEMV    (1U << 4)

/* 加速器硬件能力查询结构体（严格 64 字节） */
struct nos_accel_caps {
    uint32_t accel_type;        /* NOS_ACCEL_TYPE_* */
    uint32_t core_count;        /* 物理加速核心数量 */
    uint32_t stream_id;         /* 绑定硬件 IOMMU StreamID */
    uint32_t flags;             /* NOS_ACCEL_CAP_* */
    uint64_t supported_kernels; /* 位掩码 (1ULL << NOS_ACCEL_KERNEL_*) */
    uint64_t peak_tops_x100;    /* 峰值整型算力（TOPS * 100） */
    uint64_t max_tensor_bytes;  /* 单次作业允许的最大张量字节数 */
    char     name[24];          /* 加速器型号名称 */
};

/* 加速器提交作业描述符：严格 64 字节（零隐式填充） */
struct nos_accel_job_desc {
    uint32_t kernel_op;         /* NOS_ACCEL_KERNEL_* */
    uint32_t flags;             /* 控制标志 */
    uint64_t req_id;            /* 请求关联 ID */
    uint32_t weight_vmo_cap;    /* 只读权重 VMO 句柄（部分算子可选） */
    uint32_t input_vmo_cap;     /* 只读输入张量 VMO 句柄 */
    uint32_t output_vmo_cap;    /* 可写输出张量 VMO 句柄（严禁 Sealed） */
    uint32_t notify_cap;        /* 完成后异步唤醒的 Notification 句柄 */
    uint32_t weight_offset;     /* 权重字节偏移 */
    uint32_t input_offset;      /* 输入字节偏移 */
    uint32_t output_offset;     /* 输出字节偏移 */
    uint32_t dim_m;             /* 行数 M / 元素数 N */
    uint32_t dim_n;             /* 列数 N */
    uint32_t dim_k;             /* 归约维度 K */
    uint64_t deadline_ns;       /* 端到端截止期（触发 Deadline Inheritance） */
};

/* 加速器作业完成结果：严格 32 字节 */
struct nos_accel_job_result {
    uint64_t req_id;            /* 对应作业 req_id */
    int32_t  status;            /* 0 或 -errno */
    uint32_t output_bytes;      /* 实际写出结果字节数 */
    uint64_t elapsed_ns;        /* 执行耗时（纳秒） */
    uint64_t reserved;          /* 对齐保留字段 */
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct nos_cell_budget) == 40, "nos_cell_budget ABI 尺寸必须为 40 字节");
_Static_assert(sizeof(struct nos_cell_telemetry) == 96, "nos_cell_telemetry ABI 尺寸必须为 96 字节");
_Static_assert(sizeof(struct nos_audit_record) == 64, "nos_audit_record ABI 尺寸必须为 64 字节");
_Static_assert(sizeof(struct nos_vmo_create_args) == 32, "nos_vmo_create_args ABI 尺寸必须为 32 字节");
_Static_assert(sizeof(struct nos_ring_header) == 128, "nos_ring_header ABI 尺寸必须为 128 字节");
_Static_assert(sizeof(struct nos_ring_create_args) == 32, "nos_ring_create_args ABI 尺寸必须为 32 字节");
_Static_assert(sizeof(struct nos_ring_sqe) == 64, "nos_ring_sqe ABI 尺寸必须为 64 字节");
_Static_assert(sizeof(struct nos_ring_cqe) == 32, "nos_ring_cqe ABI 尺寸必须为 32 字节");
_Static_assert(sizeof(struct nos_infer_sched_param) == 32, "nos_infer_sched_param ABI 尺寸必须为 32 字节");
_Static_assert(sizeof(struct nos_cpu_topo_info) == 32, "nos_cpu_topo_info ABI 尺寸必须为 32 字节");
_Static_assert(sizeof(struct nos_accel_caps) == 64, "nos_accel_caps ABI 尺寸必须为 64 字节");
_Static_assert(sizeof(struct nos_accel_job_desc) == 64, "nos_accel_job_desc ABI 尺寸必须为 64 字节");
_Static_assert(sizeof(struct nos_accel_job_result) == 32, "nos_accel_job_result ABI 尺寸必须为 32 字节");
#endif

#endif /* __NOS_UAPI_AI_H__ */
