#ifndef _SIGNAL_H
#define _SIGNAL_H
#include "_q2.h"
typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 18 /* MIPS */
#define SIGKILL 9
sighandler_t signal(int, sighandler_t);
int kill(pid_t, int);
#endif
