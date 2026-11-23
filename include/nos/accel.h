/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL1 混合内核加速器队列框架 (AccelQueue / NOS_CAP_ACCEL)
 */

#ifndef __NOS_ACCEL_H__
#define __NOS_ACCEL_H__

#include <nos/object.h>
#include <uapi/nos/ai.h>
#include <stdbool.h>
#include <stdint.h>

struct nos_accel_device;
struct nos_vmo;
struct task_struct;

struct nos_accel_ops {
    int (*query_caps)(struct nos_accel_device *dev, struct nos_accel_caps *out_caps);
    int (*bind_vmo)(struct nos_accel_device *dev, struct nos_vmo *vmo, uint32_t pasid);
    int (*submit_job)(struct nos_accel_device *dev,
                      struct task_struct *caller,
                      const struct nos_accel_job_desc *desc,
                      struct nos_vmo *weight_vmo,
                      struct nos_vmo *input_vmo,
                      struct nos_vmo *output_vmo,
                      struct nos_accel_job_result *out_res);
    int (*cancel_job)(struct nos_accel_device *dev, uint64_t req_id);
    int (*wait_job)(struct nos_accel_device *dev,
                    uint64_t req_id,
                    struct nos_accel_job_result *out_res,
                    uint32_t timeout_ms);
    int (*reset_ctx)(struct nos_accel_device *dev, uint32_t pasid);
};

struct nos_accel_device {
    struct nos_object            header;     /* 必须作为首字段，支持 NOS_CAP_ACCEL 对象引用与关闭 */
    struct nos_accel_caps        caps;
    const struct nos_accel_ops  *ops;
    void                        *priv;
    bool                         registered;
    uint32_t                     index;
    uint64_t                     jobs_completed;
    uint64_t                     jobs_failed;
};

int nos_accel_register(struct nos_accel_device *dev,
                       const struct nos_accel_caps *caps,
                       const struct nos_accel_ops *ops,
                       void *priv);
void nos_accel_unregister(struct nos_accel_device *dev);

struct nos_accel_device *nos_accel_find_by_type(uint32_t accel_type);
struct nos_accel_device *nos_accel_get_default(void);

int nos_accel_query_caps(struct nos_accel_device *dev, struct nos_accel_caps *out_caps);
int nos_accel_bind_vmo(struct nos_accel_device *dev,
                       struct task_struct *caller,
                       nos_handle_t vmo_cap);

int nos_accel_submit(struct nos_accel_device *dev,
                     struct task_struct *caller,
                     const struct nos_accel_job_desc *desc,
                     struct nos_accel_job_result *out_res);

int nos_accel_wait_job(struct nos_accel_device *dev,
                       struct task_struct *caller,
                       uint64_t req_id,
                       struct nos_accel_job_result *out_res,
                       uint32_t timeout_ms);

int nos_accel_cancel_job(struct nos_accel_device *dev, uint64_t req_id);

int nos_accel_reset_ctx(struct nos_accel_device *dev,
                        struct task_struct *caller,
                        uint32_t pasid);

int nos_accel_cancel_by_obj(void *accel_obj, uint64_t job_id);

int nos_accel_dispatch_ring_sqe(void *accel_obj,
                                struct task_struct *caller,
                                const struct nos_ring_sqe *sqe,
                                struct nos_ring_cqe *out_cqe);

/**
 * 纯整数无浮点张量核函数（供 EL1 virtio-accel 与仿真验证共享，严禁碰 NEON/FP）
 */
int nos_accel_exec_cpu_kernel(uint32_t kernel_op,
                              const void *w_buf,
                              const void *in_buf,
                              void *out_buf,
                              uint32_t dim_m,
                              uint32_t dim_n,
                              uint32_t dim_k,
                              uint32_t *out_bytes);

#ifdef __HOST_TEST__
void nos_accel_host_reset(void);
#endif

#endif /* __NOS_ACCEL_H__ */
