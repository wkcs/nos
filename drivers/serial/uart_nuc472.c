/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu <huqihan@live.com>
 *
 * NUC472 UART0 中断驱动、环形 FIFO 缓冲与 Panic 紧急输出实现
 */
#include <drivers/serial.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <nos/sem.h>
#include <soc/nuc472_soc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CONFIG_UART_RX_BUFFER_SIZE
#define CONFIG_UART_RX_BUFFER_SIZE 256
#endif
#ifndef CONFIG_UART_TX_BUFFER_SIZE
#define CONFIG_UART_TX_BUFFER_SIZE 256
#endif
#ifndef CONFIG_UART_POLL_TIMEOUT
#define CONFIG_UART_POLL_TIMEOUT 1000000U
#endif

#define NUC472_UART_RX_BUF_SIZE CONFIG_UART_RX_BUFFER_SIZE
#define NUC472_UART_TX_BUF_SIZE CONFIG_UART_TX_BUFFER_SIZE

#define NUC472_UART_BAUD_MODE2_DIVIDER(clk_hz, baud) \
    ((((clk_hz) + ((baud) / 2U)) / (baud)) - 2U)

struct uart_nuc472_config {
    NUC472_UART_Type *regs;
    uint32_t irq;
    uint32_t baud_rate;
};

struct uart_nuc472_data {
    uint8_t rx_buf[NUC472_UART_RX_BUF_SIZE];
    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t rx_count;

    uint8_t tx_buf[NUC472_UART_TX_BUF_SIZE];
    uint16_t tx_head;
    uint16_t tx_tail;
    uint16_t tx_count;

    struct nos_sem rx_sem;
    bool in_panic;
    bool initialized;

    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t flow_ctrl;
};

static struct uart_nuc472_data uart0_data;

#ifdef NUC472_HOST_TEST
uint8_t host_test_uart_read_dat(void);
void host_test_uart_write_dat(uint8_t val);
void host_test_uart_clear_fifosts(uint32_t flags);
#define uart_read_dat() host_test_uart_read_dat()
#define uart_write_dat(val) host_test_uart_write_dat(val)
#define uart_clear_fifosts(flags) host_test_uart_clear_fifosts(flags)
#else
#define uart_read_dat() ((uint8_t)NUC472_UART0->DAT)
#define uart_write_dat(val) (NUC472_UART0->DAT = (val))
#define uart_clear_fifosts(flags) (NUC472_UART0->FIFOSTS = (flags))
#endif

/**
 * 硬件轮询输出单字节 (用于 earlycon、panic 或无调度上下文)
 *
 * @param dev: 设备对象结构体指针
 * @param ch: 待发送字符
 * @return: 0 成功，-ETIMEDOUT 超时
 */
static int nuc472_poll_out(const struct device *dev, unsigned char ch)
{
    uint32_t remaining = CONFIG_UART_POLL_TIMEOUT;
    (void)dev;

    while ((NUC472_UART0->FIFOSTS & NUC472_UART_FIFOSTS_TXFULL) != 0U) {
        if (remaining-- == 0U) {
            return -ETIMEDOUT;
        }
    }
    uart_write_dat((uint8_t)ch);
    return 0;
}

/**
 * 硬件轮询接收单字节
 *
 * @param dev: 设备对象结构体指针
 * @param ch: 接收字符存储指针
 * @return: 0 成功读取，-EINVAL 参数非法，-EAGAIN 硬件 FIFO 为空
 */
static int nuc472_poll_in(const struct device *dev, unsigned char *ch)
{
    (void)dev;
    if (ch == NULL) {
        return -EINVAL;
    }
    if ((NUC472_UART0->FIFOSTS & NUC472_UART_FIFOSTS_RXEMPTY) != 0U) {
        return -EAGAIN;
    }
    *ch = (unsigned char)uart_read_dat();
    return 0;
}

/**
 * 驱动流式发送字符缓冲
 *
 * 将数据推入软件 TX 环形队列并开启 THRE 中断由 ISR 异步吐向硬件 FIFO。
 * 若系统处于 panic 或未就绪状态则回退到同步轮询发送。
 *
 * @param dev: 设备对象结构体指针
 * @param buf: 发送数据缓冲区
 * @param len: 发送字节数
 * @return: 实际写入缓冲或发送的字节数，或负数错误码
 */
static ssize_t nuc472_write(const struct device *dev, const unsigned char *buf, size_t len)
{
    struct uart_nuc472_data *data = dev->data;
    size_t i;

    if (buf == NULL && len != 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }

    if (data->in_panic || !data->initialized) {
        for (i = 0; i < len; i++) {
            int rc = nuc472_poll_out(dev, buf[i]);
            if (rc < 0) {
                return i != 0 ? (ssize_t)i : rc;
            }
        }
        return (ssize_t)i;
    }

    addr_t flags = disable_irq_save();
    for (i = 0; i < len; i++) {
        if (data->tx_count >= NUC472_UART_TX_BUF_SIZE) {
            break;
        }
        data->tx_buf[data->tx_head] = buf[i];
        data->tx_head = (data->tx_head + 1U) % NUC472_UART_TX_BUF_SIZE;
        data->tx_count++;
    }
    if (data->tx_count > 0U) {
        NUC472_UART0->INTEN |= NUC472_UART_INTEN_THREIEN;
    }
    enable_irq_save(flags);

    return (ssize_t)i;
}

/**
 * 驱动流式接收字符缓冲
 *
 * 从软件 RX 环形队列中尽可能读取数据；若队列为空且 wait=true，
 * 阻塞等待二值信号量事件唤醒，并在被唤醒后重新检查并排干当前缓冲。
 *
 * @param dev: 设备对象结构体指针
 * @param buf: 目标接收缓冲指针
 * @param len: 请求读取的最大字节数
 * @param wait: 是否阻塞等待新数据
 * @return: 实际读取的字节数，或负数错误码
 */
static ssize_t nuc472_read(const struct device *dev, unsigned char *buf, size_t len, bool wait)
{
    struct uart_nuc472_data *data = dev->data;

    if (buf == NULL && len != 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }

    if (data->in_panic || !data->initialized) {
        size_t i = 0;
        while (i < len) {
            int rc = nuc472_poll_in(dev, &buf[i]);
            if (rc == 0) {
                i++;
            } else if (i != 0) {
                break;
            } else if (!wait) {
                return rc;
            }
        }
        return (ssize_t)i;
    }

    for (;;) {
        size_t received = 0;
        addr_t flags = disable_irq_save();
        while (received < len && data->rx_count > 0U) {
            buf[received++] = data->rx_buf[data->rx_tail];
            data->rx_tail = (data->rx_tail + 1U) % NUC472_UART_RX_BUF_SIZE;
            data->rx_count--;
        }
        enable_irq_save(flags);

        if (received != 0U) {
            return (ssize_t)received;
        }
        if (!wait || in_interrupt()) {
            return -EAGAIN;
        }

        int rc = nos_sem_take(&data->rx_sem, NOS_WAIT_FOREVER);
        if (rc < 0) {
            return rc;
        }
    }
}

/**
 * 进入内核紧急 Panic 模式
 *
 * 标记驱动进入 panic 状态，立即禁止硬件中断并准备切换至安全轮询输出。
 *
 * @param dev: 设备对象结构体指针
 */
static void nuc472_panic_begin(const struct device *dev)
{
    struct uart_nuc472_data *data = dev->data;
    data->in_panic = true;
    NUC472_UART0->INTEN = 0U;
}

/**
 * Panic 模式下的同步紧急输出
 *
 * 先将 TX 环形队列中残留的字符轮询吐出硬件 FIFO，随后同步发送指定的 panic 日志缓冲区。
 *
 * @param dev: 设备对象结构体指针
 * @param buf: 待发送数据缓冲区
 * @param len: 数据长度
 */
static void nuc472_panic_write(const struct device *dev, const unsigned char *buf, size_t len)
{
    struct uart_nuc472_data *data = dev->data;

    while (data->tx_count > 0U) {
        uint8_t ch = data->tx_buf[data->tx_tail];
        data->tx_tail = (data->tx_tail + 1U) % NUC472_UART_TX_BUF_SIZE;
        data->tx_count--;
        if (nuc472_poll_out(dev, ch) < 0) {
            break;
        }
    }

    if (buf != NULL) {
        for (size_t i = 0; i < len; i++) {
            if (nuc472_poll_out(dev, buf[i]) < 0) {
                break;
            }
        }
    }
}

/**
 * 动态配置 UART 硬件波特率与帧格式 (8-N-1 等)
 *
 * @param dev: 设备对象结构体指针
 * @param cfg: 串口配置参数结构体
 * @return: 0 成功，-EINVAL 参数非法，-ENOTSUP 暂不支持的配置
 */
static int nuc472_configure(const struct device *dev, const struct uart_config *cfg)
{
    struct uart_nuc472_data *data = dev->data;

    if (cfg == NULL || cfg->baud_rate == 0U) {
        return -EINVAL;
    }
    if (cfg->data_bits != UART_DATA_BITS_8 ||
        cfg->stop_bits != UART_STOP_BITS_1 ||
        cfg->flow_ctrl != UART_FLOW_NONE) {
        return -ENOTSUP;
    }

    uint32_t line = NUC472_UART_LINE_WLS_8BIT;
    if (cfg->parity == UART_PARITY_ODD) {
        line |= (1U << 3);
    } else if (cfg->parity == UART_PARITY_EVEN) {
        line |= (1U << 3) | (1U << 4);
    } else if (cfg->parity != UART_PARITY_NONE) {
        return -EINVAL;
    }

    uint32_t clk_hz = nuc472_clk_get_uart0_hz();
    if (clk_hz == 0U) {
        clk_hz = 12000000UL;
    }
    uint32_t brd = NUC472_UART_BAUD_MODE2_DIVIDER(clk_hz, cfg->baud_rate);

    NUC472_UART0->BAUD = NUC472_UART_BAUD_MODE2 | (brd & 0xFFFFUL);
    NUC472_UART0->LINE = line;

    data->baud_rate = cfg->baud_rate;
    data->data_bits = cfg->data_bits;
    data->parity = cfg->parity;
    data->stop_bits = cfg->stop_bits;
    data->flow_ctrl = cfg->flow_ctrl;
    return 0;
}

/**
 * 查询当前生效的 UART 硬件配置参数
 *
 * @param dev: 设备对象结构体指针
 * @param cfg: 存储当前配置参数的结构体指针
 * @return: 0 成功，-EINVAL 参数非法
 */
static int nuc472_get_config(const struct device *dev, struct uart_config *cfg)
{
    struct uart_nuc472_data *data = dev->data;

    if (cfg == NULL) {
        return -EINVAL;
    }
    cfg->baud_rate = data->baud_rate != 0U ? data->baud_rate : 115200U;
    cfg->data_bits = data->data_bits != 0U ? data->data_bits : UART_DATA_BITS_8;
    cfg->parity = data->parity;
    cfg->stop_bits = data->stop_bits != 0U ? data->stop_bits : UART_STOP_BITS_1;
    cfg->flow_ctrl = data->flow_ctrl;
    return 0;
}

/**
 * UART0 中断服务函数
 *
 * 处理接收可用 (RDA)、接收超时 (RXTO)、发送保持寄存器空中断 (THRE) 及线路状态错误 (RLS)。
 * RX 分支在外层关中断批量排空硬件 FIFO 入环，仅在写入新字节时单次 give 二值信号量。
 */
void UART0_IRQHandler(void);
void UART0_IRQHandler(void)
{
    struct uart_nuc472_data *data = &uart0_data;
    uint32_t intsts = NUC472_UART0->INTSTS;

    if ((intsts & (NUC472_UART_INTSTS_RDAINT | NUC472_UART_INTSTS_RXTOINT)) != 0U) {
        bool has_new = false;
        addr_t flags = disable_irq_save();
        while ((NUC472_UART0->FIFOSTS & NUC472_UART_FIFOSTS_RXEMPTY) == 0U) {
            uint8_t ch = uart_read_dat();
            if (data->rx_count < NUC472_UART_RX_BUF_SIZE) {
                data->rx_buf[data->rx_head] = ch;
                data->rx_head = (data->rx_head + 1U) % NUC472_UART_RX_BUF_SIZE;
                data->rx_count++;
                has_new = true;
            }
        }
        enable_irq_save(flags);

        if (has_new) {
            (void)nos_sem_give(&data->rx_sem);
        }
    }

    if ((intsts & NUC472_UART_INTSTS_THREINT) != 0U) {
        addr_t flags = disable_irq_save();
        uint32_t max_burst = 16U;
        while (data->tx_count > 0U && max_burst-- > 0U &&
               (NUC472_UART0->FIFOSTS & NUC472_UART_FIFOSTS_TXFULL) == 0U) {
            uint8_t ch = data->tx_buf[data->tx_tail];
            data->tx_tail = (data->tx_tail + 1U) % NUC472_UART_TX_BUF_SIZE;
            data->tx_count--;
            uart_write_dat(ch);
        }
        if (data->tx_count == 0U) {
            NUC472_UART0->INTEN &= ~NUC472_UART_INTEN_THREIEN;
        }
        enable_irq_save(flags);
    }

    if ((intsts & NUC472_UART_INTSTS_RLSINT) != 0U) {
        uart_clear_fifosts(NUC472_UART_FIFOSTS_RXOVIF |
                           NUC472_UART_FIFOSTS_BIF |
                           NUC472_UART_FIFOSTS_FEF |
                           NUC472_UART_FIFOSTS_PEF);
        if ((NUC472_UART0->FIFOSTS & NUC472_UART_FIFOSTS_RXEMPTY) == 0U) {
            (void)uart_read_dat();
        }
    }
}

/**
 * NUC472 UART0 硬件与软件环形缓冲区初始化
 *
 * @param dev: 设备对象结构体指针
 * @return: 0 成功，负数失败
 */
int uart_nuc472_init(struct device *dev)
{
    struct uart_nuc472_data *data = dev->data;
    const struct uart_nuc472_config *cfg = dev->config;

    data->rx_head = 0U;
    data->rx_tail = 0U;
    data->rx_count = 0U;
    data->tx_head = 0U;
    data->tx_tail = 0U;
    data->tx_count = 0U;
    data->in_panic = false;

    /* 将 rx_sem 初始化为事件唤醒型二值信号量 (上限 1) */
    int rc = nos_sem_init(&data->rx_sem, 0U, 1U);
    if (rc < 0) {
        return rc;
    }

    uint32_t clk_hz = nuc472_clk_get_uart0_hz();
    if (clk_hz == 0U) {
        clk_hz = NUC472_HXT_HZ;
    }

    /* 根据时钟模块配置选择 UART0 时钟源，严格保留 HIRC 回退设置，禁止强制覆盖为 HXT */
    uint32_t uartsel = (clk_hz == NUC472_HIRC_HZ) ?
                       NUC472_CLK_CLKSEL1_UARTSEL_HIRC :
                       NUC472_CLK_CLKSEL1_UARTSEL_HXT;

    nuc472_sys_unlock();
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_UART0CKEN;
    NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_UARTSEL_Msk) | uartsel;
    NUC472_CLK->CLKDIV0 &= ~NUC472_CLK_CLKDIV0_UARTDIV_Msk;
    nuc472_mfp_set(&NUC472_SYS->GPG_MFPL,
                   NUC472_SYS_MFPL_Msk(1U) | NUC472_SYS_MFPL_Msk(2U),
                   NUC472_SYS_MFPL_Val(1U, 1U) | NUC472_SYS_MFPL_Val(2U, 1U));
    nuc472_sys_lock();

    NUC472_UART0->FUNCSEL = 0U;
    NUC472_UART0->FIFO = NUC472_UART_FIFO_RXRST | NUC472_UART_FIFO_TXRST;
    NUC472_UART0->LINE = NUC472_UART_LINE_WLS_8BIT;

    uint32_t baud = (cfg != NULL && cfg->baud_rate != 0U) ? cfg->baud_rate : 115200UL;
    uint32_t brd = NUC472_UART_BAUD_MODE2_DIVIDER(clk_hz, baud);
    NUC472_UART0->BAUD = NUC472_UART_BAUD_MODE2 | (brd & 0xFFFFUL);

    NUC472_UART0->TOUT = 0x40U;
    uart_clear_fifosts(NUC472_UART_FIFOSTS_RXOVIF |
                       NUC472_UART_FIFOSTS_BIF |
                       NUC472_UART_FIFOSTS_FEF |
                       NUC472_UART_FIFOSTS_PEF);

    NUC472_UART0->INTEN = NUC472_UART_INTEN_RDAIEN |
                          NUC472_UART_INTEN_RXTOIEN |
                          NUC472_UART_INTEN_TOCNTEN |
                          NUC472_UART_INTEN_RLSIEN;

#ifndef NUC472_HOST_TEST
    nuc472_nvic_set_priority(104U, 3U);
    nuc472_nvic_enable_irq(104U);
#endif

    data->baud_rate = baud;
    data->data_bits = UART_DATA_BITS_8;
    data->parity = UART_PARITY_NONE;
    data->stop_bits = UART_STOP_BITS_1;
    data->flow_ctrl = UART_FLOW_NONE;
    data->initialized = true;

    return 0;
}

static const struct uart_driver_api uart_nuc472_api = {
    .poll_out = nuc472_poll_out,
    .poll_in = nuc472_poll_in,
    .write = nuc472_write,
    .read = nuc472_read,
    .configure = nuc472_configure,
    .get_config = nuc472_get_config,
    .panic_begin = nuc472_panic_begin,
    .panic_write = nuc472_panic_write,
};

static const struct uart_nuc472_config uart0_cfg = {
    .regs = NUC472_UART0,
    .irq = 104U,
    .baud_rate = 115200U,
};

DEVICE_DEFINE(nuc472_uart0, "UART1", uart_nuc472_init, &uart0_cfg, &uart0_data,
              &uart_nuc472_api, PRE_KERNEL_1);
