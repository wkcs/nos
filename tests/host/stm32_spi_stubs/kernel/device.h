#ifndef NOS_TEST_STM32_SPI_DEVICE_H
#define NOS_TEST_STM32_SPI_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
struct nos_dt_driver;
struct nos_dt_node;
struct mutex { int locked; };
struct device { const char *name; const void *config; void *data; const void *api;
                const struct nos_dt_driver *dt_driver; const struct nos_dt_node *dt_node;
                bool dt_binding; bool ready; struct mutex mutex; };
int device_init(struct device *dev);
int device_register(struct device *dev);
void mutex_lock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);
static inline int device_is_ready(const struct device *dev) { return dev && dev->ready; }
#endif
