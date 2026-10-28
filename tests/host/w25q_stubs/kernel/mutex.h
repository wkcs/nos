#ifndef NOS_TEST_W25Q_MUTEX_H
#define NOS_TEST_W25Q_MUTEX_H
#include <kernel/device.h>
int mutex_init(struct mutex *mutex);
void mutex_lock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);
#endif
