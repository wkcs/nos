#ifndef NOS_TEST_STM32_SPI_DEVICE_H
#define NOS_TEST_STM32_SPI_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
struct mutex { int locked; };
struct device { const char *name; const void *config; void *data; const void *api;
                bool ready; struct mutex mutex; };
int device_init(struct device *dev);
int device_register(struct device *dev);
void mutex_lock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);
static inline int device_is_ready(const struct device *dev) { return dev && dev->ready; }
#endif
