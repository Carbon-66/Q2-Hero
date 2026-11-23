#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include "../_q2.h"
#define WNOHANG 1
pid_t waitpid(pid_t, int *, int);
#endif
