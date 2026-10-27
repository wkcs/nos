set(FS_SRCS
    vfs.c
    ramfs.c
    procfs.c
    sysfs.c
)

if(CONFIG_FATFS)
    list(APPEND FS_SRCS
        vfs_smoke.c
        fatfs/diskio.c
        fatfs/ff.c
        fatfs/option/cc936.c
        fatfs/fatfs_vfs.c
    )
endif()

add_library(nos_fs OBJECT ${FS_SRCS})
nos_configure_kernel_target(nos_fs)
