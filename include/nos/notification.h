/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 64 位异步位图通知 (nos_notify) 核心接口定义
 * 专用于硬件外设中断向工作线程发送异步事件标志，支持高频同位合并。
 */

#ifndef __NOS_IPC_NOTIFICATION_H__
#define __NOS_IPC_NOTIFICATION_H__

#include <stdint.h>
#include <stdbool.h>
#include <nos/object.h>
#include <nos/wait.h>
#include <nos/timeout.h>

/**
 * 64 位异步位图通知结构体
 */
struct nos_notification {
    struct nos_object     object;        /* 通用对象生命周期管理头 (包含对象自旋锁与引用计数) */
    uint64_t              pending_bits;  /* 累积未处理的位图事件 */
    struct nos_wait_queue wait_queue;    /* 按任务实时优先级排队的等待队列 */
};

typedef struct nos_notification nos_notification_t;

/**
 * 初始化 64 位异步位图通知
 *
 * @param notif: 待初始化的通知对象指针
 * @return: 0 成功，-EINVAL 参数非法
 */
int nos_notification_init(struct nos_notification *notif);

/**
 * 触发异步位图通知 (ISR 安全)
 *
 * 累积位图事件 bits (pending_bits |= bits)。
 * 若有等待任务，交付累积位图至最高优先级等待者，清零 pending_bits，并将等待者加入延后唤醒批次。
 *
 * @param notif: 目标通知对象指针
 * @param bits: 发送的事件位掩码 (必须 != 0)
 * @return: 0 成功，-EINVAL 参数非法，-EPIPE 通知对象已被关闭
 */
int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits);

/**
 * 等待异步位图通知 (带超时与原子读清 Read-and-Clear)
 *
 * 若 pending_bits != 0，原子读出至 *out_bits 并将内部 pending_bits 清零，立即返回 0；
 * 若 pending_bits == 0 且 timeout == NOS_NO_WAIT，立即返回 -EAGAIN；
 * 否则任务挂入等待队列阻塞休眠，直到被 signal 唤醒、超时或对象被关闭。
 *
 * @param notif: 目标通知对象指针
 * @param out_bits: 接收位图的输出指针
 * @param timeout: 超时设置 (NOS_NO_WAIT, NOS_WAIT_FOREVER 或绝对 ticks)
 * @return: 0 成功获取事件位，-EINVAL 参数非法，-EAGAIN 无事件(非阻塞)，
 *          -ETIMEDOUT 等待超时，-EPIPE 通知对象已被关闭
 */
int nos_notify_wait_obj(struct nos_notification *notif, uint64_t *out_bits, nos_timeout_t timeout);

/**
 * 关闭通知对象
 *
 * 将对象标记为 closed，唤醒等待队列中的所有等待者并赋返回值 -EPIPE。
 *
 * @param notif: 目标通知对象指针
 * @return: 0 成功，-EINVAL 参数非法
 */
int nos_notification_close(struct nos_notification *notif);

#endif /* __NOS_IPC_NOTIFICATION_H__ */
