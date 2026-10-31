#ifndef PT_PTHREAD_H
#define PT_PTHREAD_H
/* The part of POSIX threads the render workers use, on Win32 slim reader/writer locks and condition variables
 * (compat_win32.c). C11 <threads.h> has no static mutex initializer, which document.c relies on. */
typedef void *pthread_t;

/* an SRWLOCK and a CONDITION_VARIABLE: one pointer each, zero when unused */
typedef struct {
    void *p;
} pthread_mutex_t;

typedef struct {
    void *p;
} pthread_cond_t;

#define PTHREAD_MUTEX_INITIALIZER \
    {                             \
        0                         \
    }
#define PTHREAD_COND_INITIALIZER \
    {                            \
        0                        \
    }

int pthread_create(pthread_t *thread, const void *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **result);
int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
#endif
