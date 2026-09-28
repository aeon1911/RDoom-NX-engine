/* Minimal glib replacement for FluidSynth on platforms without glib
 * (Nintendo Switch / newlib + pthreads). Mirrors win32_glibstubs.h. */
#ifndef _POSIX_GLIBSTUBS_H
#define _POSIX_GLIBSTUBS_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <alloca.h>
#include <sys/stat.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
typedef int gboolean;

#define GLIB_CHECK_VERSION(x, y, z) 0 /* use FluidSynth's "old" glib thread API */
#define GLIB_MAJOR_VERSION 2
#define GLIB_MINOR_VERSION 29

typedef struct
{
    int code;
    const char *message;
} GError;
typedef void *gpointer;

#define g_new(s, c) FLUID_ARRAY(s, c)
#define g_free(p) FLUID_FREE(p)
#define g_strfreev FLUID_FREE
#define g_newa(_type, _len) (_type *)alloca(sizeof(_type) * (_len))
#define g_assert(a) assert(a)
#define G_LIKELY(expr) __builtin_expect(!!(expr), 1)
#define G_UNLIKELY(expr) __builtin_expect(!!(expr), 0)

#define g_vsnprintf(b, c, f, a) vsnprintf(b, c, f, a)
#define g_snprintf(b, c, f, ...) snprintf(b, c, f, __VA_ARGS__)
#define g_return_val_if_fail(expr, val) if (expr) {} else { return val; }
#define g_clear_error(err) do {} while (0)
#define g_stat stat

#define G_FILE_TEST_EXISTS 1
#define G_FILE_TEST_IS_REGULAR 2

#define g_file_test fluid_g_file_test
#define g_shell_parse_argv fluid_g_shell_parse_argv
gboolean fluid_g_file_test(const char *path, int flags);
gboolean fluid_g_shell_parse_argv(const char *command_line, int *argcp, char ***argvp, void *dummy);

#define g_get_monotonic_time fluid_g_get_monotonic_time
double fluid_g_get_monotonic_time(void);

/* Byte ordering */
#define G_BYTE_ORDER __BYTE_ORDER__
#define G_BIG_ENDIAN __ORDER_BIG_ENDIAN__
#define GINT32_FROM_LE(x) (x)
#define GINT16_FROM_LE(x) (x)

/* Thread support */
#define g_thread_supported() 1
#define g_thread_init(_) do {} while (0)
#define g_usleep(usecs) usleep(usecs)

typedef gpointer (*GThreadFunc)(void *data);
typedef struct
{
    GThreadFunc func;
    void *data;
    pthread_t handle;
    int joinable;
} GThread;

#define g_thread_create fluid_g_thread_create
#define g_thread_join fluid_g_thread_join
#define g_thread_self fluid_g_thread_self
GThread *fluid_g_thread_create(GThreadFunc func, void *data, gboolean joinable, GError **error);
void fluid_g_thread_join(GThread *thread);
GThread *fluid_g_thread_self(void);

/* Regular mutex */
typedef pthread_mutex_t GStaticMutex;
#define G_STATIC_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define g_static_mutex_init(_m) pthread_mutex_init((_m), NULL)
#define g_static_mutex_free(_m) pthread_mutex_destroy(_m)
#define g_static_mutex_lock(_m) pthread_mutex_lock(_m)
#define g_static_mutex_unlock(_m) pthread_mutex_unlock(_m)

/* Recursive lock capable mutex */
typedef pthread_mutex_t GStaticRecMutex;
void fluid_g_rec_mutex_init(pthread_mutex_t *m);
#define g_static_rec_mutex_init(_m) fluid_g_rec_mutex_init(_m)
#define g_static_rec_mutex_free(_m) pthread_mutex_destroy(_m)
#define g_static_rec_mutex_lock(_m) pthread_mutex_lock(_m)
#define g_static_rec_mutex_unlock(_m) pthread_mutex_unlock(_m)

/* Dynamically allocated mutex suitable for fluid_cond_t use */
typedef pthread_mutex_t GMutex;
#define g_mutex_free(m) do { if (m != NULL) { pthread_mutex_destroy(m); g_free(m); } } while(0)
#define g_mutex_lock(m) pthread_mutex_lock(m)
#define g_mutex_unlock(m) pthread_mutex_unlock(m)

static inline GMutex *g_mutex_new(void)
{
    GMutex *mutex = (GMutex *)malloc(sizeof(GMutex));
    pthread_mutex_init(mutex, NULL);
    return mutex;
}

/* Thread condition signaling */
typedef pthread_cond_t GCond;
#define g_cond_free(cond) do { if (cond != NULL) { pthread_cond_destroy(cond); g_free(cond); } } while (0)
#define g_cond_signal(cond) pthread_cond_signal(cond)
#define g_cond_broadcast(cond) pthread_cond_broadcast(cond)
#define g_cond_wait(cond, mutex) pthread_cond_wait(cond, mutex)

static inline GCond *g_cond_new(void)
{
    GCond *cond = (GCond *)malloc(sizeof(GCond));
    pthread_cond_init(cond, NULL);
    return cond;
}

/* Thread private data */
typedef pthread_key_t GStaticPrivate;
#define g_static_private_init(_priv) pthread_key_create((_priv), NULL)
#define g_static_private_get(_priv) pthread_getspecific(*(_priv))
#define g_static_private_set(_priv, _data, _) pthread_setspecific(*(_priv), (_data))
#define g_static_private_free(_priv) pthread_key_delete(*(_priv))

/* Atomic operations */
#define g_atomic_int_inc(_pi) __atomic_add_fetch((_pi), 1, __ATOMIC_SEQ_CST)
#define g_atomic_int_get(_pi) __atomic_load_n((_pi), __ATOMIC_SEQ_CST)
#define g_atomic_int_set(_pi, _val) __atomic_store_n((_pi), (_val), __ATOMIC_SEQ_CST)
#define g_atomic_int_add(_pi, _add) ((void)__atomic_add_fetch((_pi), (_add), __ATOMIC_SEQ_CST))
#define g_atomic_int_dec_and_test(_pi) (__atomic_sub_fetch((_pi), 1, __ATOMIC_SEQ_CST) == 0)
#define g_atomic_int_compare_and_exchange(_pi, _old, _new) fluid_g_atomic_cas((int *)(_pi), (_old), (_new))
#define g_atomic_int_exchange_and_add(_pi, _add) __atomic_fetch_add((_pi), (_add), __ATOMIC_SEQ_CST)

static inline int fluid_g_atomic_cas(int *p, int oldv, int newv)
{
    return __atomic_compare_exchange_n(p, &oldv, newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#endif
