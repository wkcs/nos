/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 有界定长消息队列 (nos_msgq) 核心接口定义
 */

#ifndef __NOS_IPC_MSGQ_H__
#define __NOS_IPC_MSGQ_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <nos/object.h>
#include <nos/wait.h>
#include <nos/timeout.h>

/**
 * 有界定长消息队列结构体
 */
struct nos_msgq {
    struct nos_object     object;        /* 通用对象生命周期管理头 (包含对象自旋锁与引用计数) */
    uint8_t              *buffer;        /* 调用方传入的静态环形缓冲首地址 */
    size_t                element_size;  /* 单条消息的定长字节数 */
    uint32_t              capacity;      /* 队列最大容量条数 */
    uint32_t              head;          /* 读索引 (队头) */
    uint32_t              tail;          /* 写索引 (队尾) */
    uint32_t              count;         /* 当前排队的消息数 */
    struct nos_wait_queue wait_send;     /* 发送者等待队列 (队列满时排队) */
    struct nos_wait_queue wait_recv;     /* 接收者等待队列 (队列空时排队) */
};

typedef struct nos_msgq nos_msgq_t;

/**
 * 初始化有界定长消息队列
 *
 * @param q: 待初始化的消息队列指针
 * @param buffer: 静态缓冲区首地址 (由调用方提供)
 * @param buffer_bytes: 缓冲区总字节大小
 * @param element_size: 单条消息的定长字节数 (必须 > 0)
 * @param capacity: 队列最大容量条数 (必须 > 0)
 * @return: 0 成功，-EINVAL 参数非法或缓冲区不足/算术溢出
 */
int nos_msgq_init(struct nos_msgq *q, void *buffer, size_t buffer_bytes,
                  size_t element_size, uint32_t capacity);

/**
 * 发送单条消息 (带超时)
 *
 * @param q: 目标消息队列指针
 * @param element: 待发送消息数据首地址
 * @param size: 待发送数据字节大小 (必须精确等于 q->element_size)
 * @param timeout: 超时设置 (NOS_NO_WAIT, NOS_WAIT_FOREVER 或绝对 ticks)
 * @return: 0 成功，-EINVAL 参数非法，-EPIPE 队列已关闭，-EAGAIN 队列满(非阻塞)，
 *          -ENOSPC 队列满(ISR 上下文)，-ETIMEDOUT 等待超时
 */
int nos_msgq_send(struct nos_msgq *q, const void *element, size_t size,
                  nos_timeout_t timeout);

/**
 * 接收单条消息 (带超时)
 *
 * @param q: 目标消息队列指针
 * @param element: 存放接收消息的缓冲区首地址
 * @param size: 接收缓冲区字节大小 (必须 >= q->element_size)
 * @param timeout: 超时设置 (NOS_NO_WAIT, NOS_WAIT_FOREVER 或绝对 ticks)
 * @return: 0 成功，-EINVAL 参数非法，-EMSGSIZE 接收缓冲区过小，-EPIPE 队列已关闭且为空，
 *          -EAGAIN 队列空(非阻塞或 ISR)，-ETIMEDOUT 等待超时
 */
int nos_msgq_recv(struct nos_msgq *q, void *element, size_t size,
                  nos_timeout_t timeout);

/**
 * 关闭消息队列
 *
 * 将对象标记为已关闭，并唤醒所有等待发送和等待接收的任务，返回值赋为 -EPIPE。
 * 具有幂等性。
 *
 * @param q: 目标消息队列指针
 * @return: 0 成功，-EINVAL 参数非法
 */
int nos_msgq_close(struct nos_msgq *q);

#endif /* __NOS_IPC_MSGQ_H__ */
