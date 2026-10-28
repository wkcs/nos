#ifndef RAMFS_TEST_FS_H
#define RAMFS_TEST_FS_H
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
typedef off_t loff_t;
typedef uint64_t u64;
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#endif
