set(FS_SRCS
    vfs.c
    vfs_fd.c
    vfs_io.c
    ramfs.c
)

if(CONFIG_PROCFS)
    list(APPEND FS_SRCS procfs.c)
endif()

if(CONFIG_SYSFS)
    list(APPEND FS_SRCS sysfs.c)
endif()

if(CONFIG_VFS_SMOKE_TEST OR (CONFIG_FATFS AND CONFIG_BOARD_QEMU_STM32))
    list(APPEND FS_SRCS vfs_smoke.c)
endif()

if(CONFIG_VFS_FD_CONCURRENCY_TEST)
    list(APPEND FS_SRCS vfs_fd_test.c)
endif()

if(CONFIG_VFS_FAT_ALIAS_TEST)
    list(APPEND FS_SRCS vfs_alias_test.c)
endif()

if(CONFIG_FATFS)
    list(APPEND FS_SRCS
        fatfs/diskio.c
        fatfs/ff.c
        fatfs/sync.c
        fatfs/option/unicode.c
        fatfs/fatfs_vfs.c
    )
endif()

if(CONFIG_LITTLEFS)
    list(APPEND FS_SRCS
        littlefs/littlefs_vfs.c
        ../libs/third_party/littlefs/lfs.c
        ../libs/third_party/littlefs/lfs_util.c
    )
endif()

add_library(nos_fs OBJECT ${FS_SRCS})
nos_configure_kernel_target(nos_fs)

if(CONFIG_LITTLEFS)
    target_include_directories(nos_fs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/littlefs")
endif()
