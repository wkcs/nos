#ifndef TEST_SHELL_PIPE_TIMEOUT_H
#define TEST_SHELL_PIPE_TIMEOUT_H
#include <stdint.h>
typedef uint32_t nos_timeout_t;
#define NOS_WAIT_FOREVER ((nos_timeout_t)-1)
#define NOS_NO_WAIT ((nos_timeout_t)0)
#endif
