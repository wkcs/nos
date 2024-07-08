
/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */
#define pr_fmt(fmt) "[SSD1106]:%s[%d]:"fmt, __func__, __LINE__

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

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_BUF_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)
#define USE_SOFT_SPI 0

#define oled_clk(value) (PAout(5) = !!(value))
#define oled_din(value) (PAout(7) = !!(value))
#define oled_rst(value) (PBout(0) = !!(value))
#define oled_dc(value)  (PBout(1) = !!(value))
#define oled_cs(value)  (PBout(2) = !!(value))

struct ssd1106_oled {
    struct mutex lock;
    struct device dev;
    //char *buf[OLED_BUF_SIZE];

    bool enable;
};

static int ssd1106_oled_spi_write_byte(u8 data)
{
#if USE_SOFT_SPI
    u8 i;

    for(i = 0; i < 8; i++) {
        oled_clk(0);
        oled_din(data & 0x80);
        oled_clk(1);
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

static int ssd1106_oled_write_cmd(u8 cmd)
{
    int rc;

    oled_dc(0);
    oled_cs(0);
    rc = ssd1106_oled_spi_write_byte(cmd);
    oled_cs(1);

    return rc;
}

static int ssd1106_oled_write_data(u8 data)
{
    int rc;

    oled_dc(1);
    oled_cs(0);
    rc = ssd1106_oled_spi_write_byte(data);
    oled_cs(1);

    return rc;
}

static void ssd1106_oled_set_pos(unsigned char x, unsigned char y)
{
    ssd1106_oled_write_cmd(0xb0 + y);
    ssd1106_oled_write_cmd(((x & 0xf0) >> 4) | 0x10);
    ssd1106_oled_write_cmd((x & 0x0f) | 0x01);
}

void ssd1106_oled_display_on(void)
{
    ssd1106_oled_write_cmd(0X8D);  //SET DCDC命令
    ssd1106_oled_write_cmd(0X14);  //DCDC ON
    ssd1106_oled_write_cmd(0XAF);  //DISPLAY ON
}

void ssd1106_oled_display_off(void)
{
    ssd1106_oled_write_cmd(0X8D);  //SET DCDC命令
    ssd1106_oled_write_cmd(0X10);  //DCDC OFF
    ssd1106_oled_write_cmd(0XAE);  //DISPLAY OFF
}

void ssd1106_oled_clear(void)
{
    u8 i, n;
    for(i = 0; i < 8; i++) {
        ssd1106_oled_write_cmd(0xb0 + i);    //设置页地址（0~7）
        ssd1106_oled_write_cmd(0x00);      //设置显示位置—列低地址
        ssd1106_oled_write_cmd(0x10);      //设置显示位置—列高地址
        for(n = 0; n < 128; n++)
            ssd1106_oled_write_data(0);
    }
}

static void ssd1106_oled_buf_init(struct ssd1106_oled *oled)
{
    mutex_lock(&oled->lock);
    //memset(oled->buf, 0, OLED_BUF_SIZE);
    mutex_unlock(&oled->lock);
}

static ssize_t ssd1106_oled_buf_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct ssd1106_oled *oled = dev->priv;
    u8 x, y;
    size_t i;
    const u8 *data = buffer;

    x = pos % OLED_WIDTH;
    y = pos / OLED_WIDTH;

    mutex_lock(&oled->lock);
    ssd1106_oled_set_pos(x, y);
    for (i = 0; i < size; i++) {
        ssd1106_oled_write_data(data[i]);
    }
    mutex_unlock(&oled->lock);

    return size;
}

static int ssd1106_oled_control(struct device *dev, int cmd, void *args)
{
    struct ssd1106_oled *oled = dev->priv;
    struct display_dev_info info = {.width = OLED_WIDTH, .height = OLED_HEIGHT};

    switch (cmd) {
    case DS_CTRL_ENABLE:
        oled->enable = true;
        break;
    case DS_CTRL_DISABLE:
        oled->enable = false;
        ssd1106_oled_buf_init(oled);
        break;
    case DS_CTRL_GET_ENABLE_STATUS:
        return oled->enable;
    case DS_CTRL_GET_DEV_INFO:
        memcpy(args, &info, sizeof(struct display_dev_info));
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static int ssd1106_oled_gpio_init(void)
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

static int ssd1106_oled_spi_init(void)
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
#endif

    return 0;
}

static int ssd1106_oled_hw_init(void)
{
    oled_rst(1);
    msleep(100);
    oled_rst(0);
    msleep(100);
    oled_rst(1);

    ssd1106_oled_write_cmd(0xAE);//--turn off oled panel
	ssd1106_oled_write_cmd(0x00);//---set low column address
	ssd1106_oled_write_cmd(0x10);//---set high column address
	ssd1106_oled_write_cmd(0x40);//--set start line address  Set Mapping RAM Display Start Line (0x00~0x3F)
	ssd1106_oled_write_cmd(0x81);//--set contrast control register
	ssd1106_oled_write_cmd(0xCF); // Set SEG Output Current Brightness
	ssd1106_oled_write_cmd(0xA1);//--Set SEG/Column Mapping     0xa0左右反置 0xa1正常
	ssd1106_oled_write_cmd(0xC8);//Set COM/Row Scan Direction   0xc0上下反置 0xc8正常
	ssd1106_oled_write_cmd(0xA6);//--set normal display
	ssd1106_oled_write_cmd(0xA8);//--set multiplex ratio(1 to 64)
	ssd1106_oled_write_cmd(0x3f);//--1/64 duty
	ssd1106_oled_write_cmd(0xD3);//-set display offset	Shift Mapping RAM Counter (0x00~0x3F)
	ssd1106_oled_write_cmd(0x00);//-not offset
	ssd1106_oled_write_cmd(0xd5);//--set display clock divide ratio/oscillator frequency
	ssd1106_oled_write_cmd(0x80);//--set divide ratio, Set Clock as 100 Frames/Sec
	ssd1106_oled_write_cmd(0xD9);//--set pre-charge period
	ssd1106_oled_write_cmd(0xF1);//Set Pre-Charge as 15 Clocks & Discharge as 1 Clock
	ssd1106_oled_write_cmd(0xDA);//--set com pins hardware configuration
	ssd1106_oled_write_cmd(0x12);
	ssd1106_oled_write_cmd(0xDB);//--set vcomh
	ssd1106_oled_write_cmd(0x40);//Set VCOM Deselect Level
	ssd1106_oled_write_cmd(0x20);//-Set Page Addressing Mode (0x00/0x01/0x02)
	ssd1106_oled_write_cmd(0x02);//
	ssd1106_oled_write_cmd(0x8D);//--set Charge Pump enable/disable
	ssd1106_oled_write_cmd(0x14);//--set(0x10) disable
	ssd1106_oled_write_cmd(0xA4);// Disable Entire Display On (0xa4/0xa5)
	ssd1106_oled_write_cmd(0xA6);// Disable Inverse Display On (0xa6/a7)
	ssd1106_oled_write_cmd(0xAF);//--turn on oled panel

	ssd1106_oled_write_cmd(0xAF); /*display ON*/
	ssd1106_oled_clear();
	ssd1106_oled_set_pos(0, 0);
}

static int ssd1106_oled_init(void)
{
    struct ssd1106_oled *oled;

    oled = kzalloc(sizeof(struct ssd1106_oled), GFP_KERNEL);
    if (oled == NULL) {
        pr_err("alloc ssd1106_oled buf error\r\n");
        return -ENOMEM;
    }

    mutex_init(&oled->lock);
    oled->enable = false;

    ssd1106_oled_gpio_init();
    ssd1106_oled_spi_init();
    ssd1106_oled_hw_init();

    device_init(&oled->dev);
    oled->dev.name = "ssd1106-oled";
    oled->dev.ops.write = ssd1106_oled_buf_write;
    oled->dev.ops.control = ssd1106_oled_control;
    oled->dev.priv = oled;
    device_register(&oled->dev);

    return 0;
}
task_init(ssd1106_oled_init);
