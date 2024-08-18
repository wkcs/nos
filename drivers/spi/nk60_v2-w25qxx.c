/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[W25QXX]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/mm.h>
#include <string.h>
#include <board/board.h>

#define W25Q80 	0XEF13
#define W25Q16 	0XEF14
#define W25Q32 	0XEF15
#define W25Q64 	0XEF16
#define W25Q128	0XEF17

#define W25QXX_CS PBout(2) //W25QXX的片选信号

// 指令表
#define W25X_WriteEnable      0x06
#define W25X_WriteDisable     0x04
#define W25X_ReadStatusReg    0x05
#define W25X_WriteStatusReg   0x01
#define W25X_ReadData         0x03
#define W25X_FastReadData     0x0B
#define W25X_FastReadDual     0x3B
#define W25X_PageProgram      0x02
#define W25X_BlockErase       0xD8
#define W25X_SectorErase      0x20
#define W25X_ChipErase        0xC7
#define W25X_PowerDown        0xB9
#define W25X_ReleasePowerDown 0xAB
#define W25X_DeviceID         0xAB
#define W25X_ManufactDeviceID 0x90
#define W25X_JedecDeviceID    0x9F

#define BLOACK_BUF_SIZE 4096
struct w25qxx_info {
    struct device dev;
    uint16_t id;
    //uint8_t buf[BLOACK_BUF_SIZE];
};

uint8_t spi1_read_write_byte(uint8_t tx_data)
{

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
    } //等待发送区空

    SPI_I2S_SendData(SPI1, tx_data); //通过外设SPIx发送一个byte  数据

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET)
    {
    } //等待接收完一个byte

    return SPI_I2S_ReceiveData(SPI1); //返回通过SPIx最近接收的数据
}

void spi1_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    SPI_InitTypeDef SPI_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_PinAFConfig(GPIOB, GPIO_PinSource3, GPIO_AF_SPI1);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource4, GPIO_AF_SPI1);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource5, GPIO_AF_SPI1);

    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, ENABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, DISABLE);

    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_256;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init(SPI1, &SPI_InitStructure);

    SPI_Cmd(SPI1, ENABLE);

    spi1_read_write_byte(0xff);
}

void spi1_set_speed(uint8_t SPI_BaudRatePrescaler)
{
    assert_param(IS_SPI_BAUDRATE_PRESCALER(SPI_BaudRatePrescaler));
    SPI1->CR1 &= 0XFFC7;                // 位3-5清零，用来设置波特率
    SPI1->CR1 |= SPI_BaudRatePrescaler; // 设置SPI1速度
    SPI_Cmd(SPI1, ENABLE);              // 使能SPI1
}

//读取W25QXX的状态寄存器
//BIT7  6   5   4   3   2   1   0
//SPR   RV  TB BP2 BP1 BP0 WEL BUSY
//SPR:默认0,状态寄存器保护位,配合WP使用
//TB,BP2,BP1,BP0:FLASH区域写保护设置
//WEL:写使能锁定
//BUSY:忙标记位(1,忙;0,空闲)
//默认:0x00
uint8_t w25qxx_read_sr(void)
{
    uint8_t byte = 0;
    W25QXX_CS = 0;                          //使能器件
    spi1_read_write_byte(W25X_ReadStatusReg); //发送读取状态寄存器命令
    byte = spi1_read_write_byte(0Xff);        //读取一个字节
    W25QXX_CS = 1;                          //取消片选
    return byte;
}
//写W25QXX状态寄存器
//只有SPR,TB,BP2,BP1,BP0(bit 7,5,4,3,2)可以写!!!
void w25qxx_write_sr(uint8_t sr)
{
    W25QXX_CS = 0;                           //使能器件
    spi1_read_write_byte(W25X_WriteStatusReg); //发送写取状态寄存器命令
    spi1_read_write_byte(sr);                  //写入一个字节
    W25QXX_CS = 1;                           //取消片选
}
//W25QXX写使能
//将WEL置位
void w25qxx_write_enable(void)
{
    W25QXX_CS = 0;                        //使能器件
    spi1_read_write_byte(W25X_WriteEnable); //发送写使能
    W25QXX_CS = 1;                        //取消片选
}
//W25QXX写禁止
//将WEL清零
void w25qxx_write_disable(void)
{
    W25QXX_CS = 0;                         //使能器件
    spi1_read_write_byte(W25X_WriteDisable); //发送写禁止指令
    W25QXX_CS = 1;                         //取消片选
}
//等待空闲
void w25qxx_wait_busy(void)
{
    while ((w25qxx_read_sr() & 0x01) == 0x01)
        msleep(1);
}
//擦除整个芯片
//等待时间超长...
void w25qxx_erase_chip(void)
{
    w25qxx_write_enable(); //SET WEL
    w25qxx_wait_busy();
    W25QXX_CS = 0;                      //使能器件
    spi1_read_write_byte(W25X_ChipErase); //发送片擦除命令
    W25QXX_CS = 1;                      //取消片选
    w25qxx_wait_busy();                 //等待芯片擦除结束
}
//擦除一个扇区
//Dst_Addr:扇区地址 根据实际容量设置
//擦除一个山区的最少时间:150ms
void w25qxx_erase_sector(uint32_t Dst_Addr)
{
    //pr_info("fe:%x\r\n", Dst_Addr);
    Dst_Addr *= BLOACK_BUF_SIZE;
    w25qxx_write_enable(); //SET WEL
    w25qxx_wait_busy();
    //pr_info("next\r\n");
    W25QXX_CS = 0;                              //使能器件
    spi1_read_write_byte(W25X_SectorErase);       //发送扇区擦除指令
    spi1_read_write_byte((uint8_t)((Dst_Addr) >> 16)); //发送24bit地址
    spi1_read_write_byte((uint8_t)((Dst_Addr) >> 8));
    spi1_read_write_byte((uint8_t)Dst_Addr);
    W25QXX_CS = 1;      //取消片选
    w25qxx_wait_busy(); //等待擦除完成
}
//进入掉电模式
void w25qxx_power_down(void)
{
    W25QXX_CS = 0;                      //使能器件
    spi1_read_write_byte(W25X_PowerDown); //发送掉电命令
    W25QXX_CS = 1;                      //取消片选
    usleep(3);                        //等待TPD
}
//唤醒
void w25qxx_wakeup(void)
{
    W25QXX_CS = 0;                             //使能器件
    spi1_read_write_byte(W25X_ReleasePowerDown); //  send W25X_PowerDown command 0xAB
    W25QXX_CS = 1;                             //取消片选
    usleep(3);                               //等待TRES1
}

//读取芯片ID
//返回值如下:
//0XEF13,表示芯片型号为W25Q80
//0XEF14,表示芯片型号为W25Q16
//0XEF15,表示芯片型号为W25Q32
//0XEF16,表示芯片型号为W25Q64
//0XEF17,表示芯片型号为W25Q128
uint16_t w25qxx_read_id(void)
{
    uint16_t Temp = 0;
    W25QXX_CS = 0;
    spi1_read_write_byte(0x90); //发送读取ID命令
    spi1_read_write_byte(0x00);
    spi1_read_write_byte(0x00);
    spi1_read_write_byte(0x00);
    Temp |= spi1_read_write_byte(0xFF) << 8;
    Temp |= spi1_read_write_byte(0xFF);
    W25QXX_CS = 1;
    return Temp;
}
//读取SPI FLASH
//在指定地址开始读取指定长度的数据
ssize_t w25qxx_read(__maybe_unused struct device *dev, addr_t pos, void *buffer, size_t size)
{
    uint16_t i;
    W25QXX_CS = 0;                              //使能器件
    spi1_read_write_byte(W25X_ReadData);          //发送读取命令
    spi1_read_write_byte((uint8_t)((pos) >> 16)); //发送24bit地址
    spi1_read_write_byte((uint8_t)((pos) >> 8));
    spi1_read_write_byte((uint8_t)pos);
    for (i = 0; i < size; i++)
        ((uint8_t *)buffer)[i] = spi1_read_write_byte(0xff); //循环读数
    W25QXX_CS = 1;

    return size;
}
//SPI在一页(0~65535)内写入少于256个字节的数据
//在指定地址开始写入最大256字节的数据
//pBuffer:数据存储区
//WriteAddr:开始写入的地址(24bit)
//NumByteToWrite:要写入的字节数(最大256),该数不应该超过该页的剩余字节数!!!
void w25qxx_write_page(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    uint16_t i;
    w25qxx_write_enable();                       //SET WEL
    W25QXX_CS = 0;                               //使能器件
    spi1_read_write_byte(W25X_PageProgram);        //发送写页命令
    spi1_read_write_byte((uint8_t)((WriteAddr) >> 16)); //发送24bit地址
    spi1_read_write_byte((uint8_t)((WriteAddr) >> 8));
    spi1_read_write_byte((uint8_t)WriteAddr);
    for (i = 0; i < NumByteToWrite; i++)
        spi1_read_write_byte(pBuffer[i]); //循环写数
    W25QXX_CS = 1;                      //取消片选
    w25qxx_wait_busy();                 //等待写入结束
}
//无检验写SPI FLASH
//必须确保所写的地址范围内的数据全部为0XFF,否则在非0XFF处写入的数据将失败!
//具有自动换页功能
//在指定地址开始写入指定长度的数据,但是要确保地址不越界!
//pBuffer:数据存储区
//WriteAddr:开始写入的地址(24bit)
//NumByteToWrite:要写入的字节数(最大65535)
//CHECK OK
void w25qxx_write_no_check(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    uint16_t pageremain;
    pageremain = 256 - WriteAddr % 256; //单页剩余的字节数
    if (NumByteToWrite <= pageremain)
        pageremain = NumByteToWrite; //不大于256个字节
    while (1)
    {
        w25qxx_write_page(pBuffer, WriteAddr, pageremain);
        if (NumByteToWrite == pageremain)
            break; //写入结束了
        else       //NumByteToWrite>pageremain
        {
            pBuffer += pageremain;
            WriteAddr += pageremain;

            NumByteToWrite -= pageremain; //减去已经写入了的字节数
            if (NumByteToWrite > 256)
                pageremain = 256; //一次可以写入256个字节
            else
                pageremain = NumByteToWrite; //不够256个字节了
        }
    };
}
//写SPI FLASH
//在指定地址开始写入指定长度的数据
//该函数带擦除操作!
ssize_t w25qxx_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    uint32_t secpos;
    uint16_t secoff;
    uint16_t secremain;
    uint16_t i;
    size_t index = 0;
    uint8_t *buf;
    struct w25qxx_info *chip_info = container_of(dev, struct w25qxx_info, dev);

    secpos = pos / BLOACK_BUF_SIZE; //扇区地址
    secoff = pos % BLOACK_BUF_SIZE; //在扇区内的偏移
    secremain = BLOACK_BUF_SIZE - secoff; //扇区剩余空间大小
    if (size <= secremain)
        secremain = size; //不大于4096个字节

    buf = kmalloc(BLOACK_BUF_SIZE, GFP_KERNEL);
    if (buf == NULL) {
        pr_err("alloc buf error\r\n");
        return -ENOMEM;
    }

    while (1)
    {
        w25qxx_read(dev, secpos * BLOACK_BUF_SIZE, (void *)buf, BLOACK_BUF_SIZE); //读出整个扇区的内容
        for (i = 0; i < secremain; i++)               //校验数据
        {
            if (buf[secoff + i] != 0XFF)
                break; //需要擦除
        }
        if (i < secremain) //需要擦除
        {
            w25qxx_erase_sector(secpos);    //擦除这个扇区
            for (i = 0; i < secremain; i++) //复制
            {
                buf[i + secoff] = ((uint8_t *)buffer)[i + index];
            }
            w25qxx_write_no_check(buf, secpos * BLOACK_BUF_SIZE, BLOACK_BUF_SIZE); //写入整个扇区
        }
        else
            w25qxx_write_no_check((uint8_t *)buffer + index, pos, secremain); //写已经擦除了的,直接写入扇区剩余区间.
        if (size == secremain)
            break; //写入结束了
        else       //写入未结束
        {
            secpos++;   //扇区地址增1
            secoff = 0; //偏移位置为0
            index += secremain;
            pos += secremain;      //写地址偏移
            size -= secremain; //字节数递减
            if (size > BLOACK_BUF_SIZE)
                secremain = BLOACK_BUF_SIZE; //下一个扇区还是写不完
            else
                secremain = size; //下一个扇区可以写完了
        }
    }
    kfree(buf);

    return size;
}

int spi_w25qxx_init(void)
{
    struct w25qxx_info *info;
    GPIO_InitTypeDef GPIO_InitStructure;

    info = kmalloc(sizeof(*info), GFP_KERNEL);
    if (info == NULL) {
        pr_err("alloc w25qxx info buf err\r\n");
        return -ENOMEM;
    }

    device_init(&info->dev);
    info->dev.name = "nk60_v2-flash";
    info->dev.ops.write = w25qxx_write;
    info->dev.ops.read = w25qxx_read;
    device_register(&info->dev);

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    W25QXX_CS = 1;
    spi1_init();
    spi1_set_speed(SPI_BaudRatePrescaler_4);
    info->id = w25qxx_read_id();
    pr_info("w25qxx: chip id = 0x%x\r\n", info->id);

    return 0;
}
task_init(spi_w25qxx_init);
