
/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */
#define pr_fmt(fmt) "[ST7789]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <board/board.h>
#include <kernel/sem.h>
#include <kernel/mutex.h>

#include <display/display.h>

#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_COLOR_WIDTH sizeof(u16)
#define LCD_BUF_SZIE (LCD_WIDTH * LCD_HEIGHT * LCD_COLOR_WIDTH)
#define USE_SOFT_SPI 0

#define lcd_clk(value) (PAout(5) = !!(value))
#define lcd_din(value) (PAout(7) = !!(value))
#define lcd_rst(value) (PBout(0) = !!(value))
#define lcd_dc(value)  (PBout(1) = !!(value))
#define lcd_blk(value) (PBout(2) = !!(value))

struct st7789_lcd {
    struct mutex lock;
    struct device dev;

    bool enable;
};

static int st7789_lcd_spi_write_byte(u8 data)
{
#if USE_SOFT_SPI
    u8 i;

    for(i = 0; i < 8; i++) {
        lcd_clk(0);
        lcd_din(data & 0x80);
        lcd_clk(1);
        data <<= 1;
    }
#else
    u8 retry = 0;

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET) {
        retry++;
        if (retry > 200)
            return -EBUSY;
    }
    SPI_I2S_SendData(SPI1, data);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET) {
        retry++;
        if (retry > 200)
            return -EBUSY;
    }
#endif

    return 0;
}

static int st7789_lcd_write_byte(u8 data)
{
    lcd_dc(1);
    return st7789_lcd_spi_write_byte(data);
}

static int st7789_lcd_write_half_word(u16 data)
{
    lcd_dc(1);
    st7789_lcd_spi_write_byte((data >> 8) & 0xff);
    return st7789_lcd_spi_write_byte(data & 0xff);
}

static int st7789_lcd_write_reg(u8 data)
{
    lcd_dc(0);
    return st7789_lcd_spi_write_byte(data);
}

static void st7789_lcd_address_set(u16 x1, u16 y1, u16 x2, u16 y2)
{
    st7789_lcd_write_reg(0x2a);
    st7789_lcd_write_half_word(x1);
    st7789_lcd_write_half_word(x2);

    st7789_lcd_write_reg(0x2b);
    st7789_lcd_write_half_word(y1);
    st7789_lcd_write_half_word(y2);

    st7789_lcd_write_reg(0x2C);
}

static void st7789_lcd_clear(u16 color)
{
    u16 i, j;

    st7789_lcd_address_set(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for(i = 0; i < LCD_WIDTH; i++) {
        for (j = 0; j < LCD_HEIGHT; j++) {
            st7789_lcd_write_half_word(color);
        }
    }
}

static void st7789_lcd_buf_init(struct st7789_lcd *lcd)
{
}

static ssize_t st7789_lcd_buf_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct st7789_lcd *lcd = dev->priv;
    u16 x_s, y_s, x_e, y_e;
    const u16 *data;
    size_t index;

    x_s = pos % LCD_WIDTH;
    y_s = pos / LCD_WIDTH;
    if (y_s >= LCD_HEIGHT)
        y_s = LCD_HEIGHT;
    x_e = (pos + size / LCD_COLOR_WIDTH) % LCD_WIDTH;
    y_e = (pos + size / LCD_COLOR_WIDTH) / LCD_WIDTH;
    if (y_e >= LCD_HEIGHT)
        y_e = LCD_HEIGHT;

    mutex_lock(&lcd->lock);
    st7789_lcd_address_set(x_s, y_s, x_e, y_e);
    data = (u16 *)buffer;
    for (index = 0; index < size / LCD_COLOR_WIDTH; index++) {
        st7789_lcd_write_half_word(data[index]);
    }
    mutex_unlock(&lcd->lock);

    return size;
}

static int st7789_lcd_control(struct device *dev, int cmd, void *args)
{
    struct st7789_lcd *lcd = dev->priv;
    struct display_dev_info info = {.width = LCD_WIDTH, .height = LCD_HEIGHT};

    switch (cmd) {
    case DS_CTRL_ENABLE:
        lcd->enable = true;
        break;
    case DS_CTRL_DISABLE:
        lcd->enable = false;
        st7789_lcd_buf_init(lcd);
        break;
    case DS_CTRL_GET_ENABLE_STATUS:
        return lcd->enable;
    case DS_CTRL_GET_DEV_INFO:
        memcpy(args, &info, sizeof(struct display_dev_info));
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static int st7789_lcd_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    GPIO_SetBits(GPIOB, GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2);

    return 0;
}

static int st7789_lcd_spi_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
#if USE_SOFT_SPI
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_SetBits(GPIOA, GPIO_Pin_5 | GPIO_Pin_7);
#else
    SPI_InitTypeDef  SPI_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_SetBits(GPIOA, GPIO_Pin_5 | GPIO_Pin_7);

    SPI_InitStructure.SPI_Direction = SPI_Direction_1Line_Tx;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init(SPI1, &SPI_InitStructure);
    SPI_Cmd(SPI1, ENABLE);
    st7789_lcd_spi_write_byte(0xff);
#endif

    return 0;
}

static int st7789_lcd_hw_init(void)
{
    lcd_rst(0);
    msleep(20);
    lcd_rst(1);
    msleep(20);
    lcd_blk(1);

    st7789_lcd_write_reg(0x36);
    st7789_lcd_write_byte(0x00);

    st7789_lcd_write_reg(0x3A);
    st7789_lcd_write_byte(0x05);

    st7789_lcd_write_reg(0xB2);
    st7789_lcd_write_byte(0x0C);
    st7789_lcd_write_byte(0x0C);
    st7789_lcd_write_byte(0x00);
    st7789_lcd_write_byte(0x33);
    st7789_lcd_write_byte(0x33);

    st7789_lcd_write_reg(0xB7);
    st7789_lcd_write_byte(0x35);

    st7789_lcd_write_reg(0xBB);
    st7789_lcd_write_byte(0x19);

    st7789_lcd_write_reg(0xC0);
    st7789_lcd_write_byte(0x2C);

    st7789_lcd_write_reg(0xC2);
    st7789_lcd_write_byte(0x01);

    st7789_lcd_write_reg(0xC3);
    st7789_lcd_write_byte(0x12);

    st7789_lcd_write_reg(0xC4);
    st7789_lcd_write_byte(0x20);

    st7789_lcd_write_reg(0xC6);
    st7789_lcd_write_byte(0x0F);

    st7789_lcd_write_reg(0xD0);
    st7789_lcd_write_byte(0xA4);
    st7789_lcd_write_byte(0xA1);

    st7789_lcd_write_reg(0xE0);
    st7789_lcd_write_byte(0xD0);
    st7789_lcd_write_byte(0x04);
    st7789_lcd_write_byte(0x0D);
    st7789_lcd_write_byte(0x11);
    st7789_lcd_write_byte(0x13);
    st7789_lcd_write_byte(0x2B);
    st7789_lcd_write_byte(0x3F);
    st7789_lcd_write_byte(0x54);
    st7789_lcd_write_byte(0x4C);
    st7789_lcd_write_byte(0x18);
    st7789_lcd_write_byte(0x0D);
    st7789_lcd_write_byte(0x0B);
    st7789_lcd_write_byte(0x1F);
    st7789_lcd_write_byte(0x23);

    st7789_lcd_write_reg(0xE1);
    st7789_lcd_write_byte(0xD0);
    st7789_lcd_write_byte(0x04);
    st7789_lcd_write_byte(0x0C);
    st7789_lcd_write_byte(0x11);
    st7789_lcd_write_byte(0x13);
    st7789_lcd_write_byte(0x2C);
    st7789_lcd_write_byte(0x3F);
    st7789_lcd_write_byte(0x44);
    st7789_lcd_write_byte(0x51);
    st7789_lcd_write_byte(0x2F);
    st7789_lcd_write_byte(0x1F);
    st7789_lcd_write_byte(0x1F);
    st7789_lcd_write_byte(0x20);
    st7789_lcd_write_byte(0x23);

    st7789_lcd_write_reg(0x21);

    st7789_lcd_write_reg(0x11);

    st7789_lcd_write_reg(0x29);
}

static int st7789_lcd_init(void)
{
    struct st7789_lcd *lcd;

    lcd = kmalloc(sizeof(struct st7789_lcd), GFP_KERNEL);
    if (lcd == NULL) {
        pr_err("alloc st7789_lcd buf error\r\n");
        return -ENOMEM;
    }

    mutex_init(&lcd->lock);
    lcd->enable = false;

    st7789_lcd_gpio_init();
    st7789_lcd_spi_init();
    st7789_lcd_hw_init();

    device_init(&lcd->dev);
    lcd->dev.name = "st7789-lcd";
    lcd->dev.ops.write = st7789_lcd_buf_write;
    lcd->dev.ops.control = st7789_lcd_control;
    lcd->dev.priv = lcd;
    device_register(&lcd->dev);

    return 0;
}
task_init(st7789_lcd_init);
