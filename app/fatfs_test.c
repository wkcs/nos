/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[fastfs_test]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <kernel/sem.h>
#include <kernel/list.h>

#include "../fs/fatfs/ff.h"

FATFS fs;					   /* FatFs文件系统对象 */
FIL fnew;					   /* 文件对象 */
FRESULT res_sd;                /* 文件操作结果 */
UINT fnum;            		   /* 文件成功读写数量 */
BYTE ReadBuffer[1024]={0};        /* 读缓冲区 */
BYTE WriteBuffer[] =              /* 写缓冲区*/
"nos fatfs文件系统测试文件\r\n";

static void fastfs_test_task_entry(void* parameter)
{
    //在外部SPI Flash挂载文件系统，文件系统挂载时会对SPI设备初始化
	res_sd = f_mount(&fs,"0:",1);
	
/*----------------------- 格式化测试 ---------------------------*/  
	/* 如果没有文件系统就格式化创建创建文件系统 */
	if(res_sd == FR_NO_FILESYSTEM)
	{
		pr_info("》SD卡还没有文件系统，即将进行格式化...\r\n");
        /* 格式化 */
		res_sd=f_mkfs("0:",0,0);							
		
		if(res_sd == FR_OK)
		{
			pr_info("》SD卡已成功格式化文件系统。\r\n");
            /* 格式化后，先取消挂载 */
			res_sd = f_mount(NULL,"0:",1);			
            /* 重新挂载	*/			
			res_sd = f_mount(&fs,"0:",1);
		}
		else
		{
			pr_info("《《格式化失败。》》res_sd =%d\r\n",res_sd);
			while(1);
		}
	}
  else if(res_sd!=FR_OK)
  {
    pr_info("！！SD卡挂载文件系统失败。(%d)\r\n",res_sd);
    pr_info("！！可能原因：SD卡初始化不成功。\r\n");
		while(1);
  }
  else
  {
    pr_info("》文件系统挂载成功，可以进行读写测试\r\n");
  }
  
/*----------------------- 文件系统测试：写测试 -----------------------------*/
	/* 打开文件，如果文件不存在则创建它 */
	pr_info("\r\n****** 即将进行文件写入测试... ******\r\n");	
	res_sd = f_open(&fnew, "0:FatFs读写测试文件.txt",FA_CREATE_ALWAYS | FA_WRITE );
	if ( res_sd == FR_OK )
	{
		pr_info("》打开/创建FatFs读写测试文件.txt文件成功，向文件写入数据。\r\n");
        /* 将指定存储区内容写入到文件内 */
        res_sd=f_write(&fnew,WriteBuffer,sizeof(WriteBuffer),&fnum);
        /* 实测SPI_SD驱动下写入大于512字节的数据在SD卡里打开会显示乱码，如需写入大量数据使用f_write_co替代上面f_write即可 */
		//res_sd=f_write_co(&fnew,WriteBuffer,sizeof(WriteBuffer),&fnum);
    if(res_sd==FR_OK)
    {
      pr_info("》文件写入成功，写入字节数据：%d\r\n",fnum);
      pr_info("》向文件写入的数据为：\r\n%s\r\n",WriteBuffer);
    }
    else
    {
      pr_info("！！文件写入失败：(%d)\r\n",res_sd);
    }    
	/* 不再读写，关闭文件 */
    f_close(&fnew);
	}
	else
	{	
		pr_info("！！打开/创建文件失败。\r\n");
	}
	
/*------------------- 文件系统测试：读测试 ------------------------------------*/
	pr_info("****** 即将进行文件读取测试... ******\r\n");
	res_sd = f_open(&fnew, "0:FatFs读写测试文件.txt", FA_OPEN_EXISTING | FA_READ); 	 
	if(res_sd == FR_OK)
	{
		pr_info("》打开文件成功。\r\n");
		res_sd = f_read(&fnew, ReadBuffer, sizeof(ReadBuffer), &fnum);
        /* 实测SPI_SD驱动下读取大于512字节的数据在SD卡里打开会显示乱码，如需读取大量数据使用f_read_co替代上面f_read即可 */
        //res_sd = f_read_co(&fnew, ReadBuffer, sizeof(ReadBuffer), &fnum);
    if(res_sd==FR_OK)
    {
      pr_info("》文件读取成功,读到字节数据：%d\r\n",fnum);
      pr_info("》读取得的文件数据为：\r\n%s \r\n", ReadBuffer);	
    }
    else
    {
      pr_info("！！文件读取失败：(%d)\r\n",res_sd);
    }		
	}
	else
	{
		pr_info("！！打开文件失败。\r\n");
	}
	/* 不再读写，关闭文件 */
	f_close(&fnew);	
  
	/* 不再使用文件系统，取消挂载文件系统 */
	f_mount(NULL,"0:",1);
}

static int fastfs_test_init(void)
{
    struct task_struct *task;

    task = task_create("fastfs_test", fastfs_test_task_entry, NULL, 15, 2048, 10, NULL);
    if (task == NULL) {
        pr_fatal("creat fastfs_test task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(task);

    return 0;
}
task_init(fastfs_test_init);
