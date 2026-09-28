#include "config.h"
#if defined(WITH_GLIB_STUBS) && !defined(WIN32)

#include "fluidsynth_priv.h"
#include "fluid_sys.h"
#include "posix_glibstubs.h"

#include <time.h>

gboolean fluid_g_file_test(const char *path, int flags)
{
    struct stat st;
    if (path == NULL || stat(path, &st) != 0)
        return FALSE;
    if (flags & G_FILE_TEST_EXISTS)
        return TRUE;
    if (flags & G_FILE_TEST_IS_REGULAR)
        return S_ISREG(st.st_mode) ? TRUE : FALSE;
    return FALSE;
}

gboolean fluid_g_shell_parse_argv(const char *command_line, int *argcp, char ***argvp, void *dummy)
{
    (void)command_line; (void)dummy;
    if (argcp) *argcp = 0;
    if (argvp) *argvp = NULL;
    return FALSE; /* the command shell is not used by ZMusic */
}

double fluid_g_get_monotonic_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

void fluid_g_rec_mutex_init(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void *g_thread_wrapper(void *info_)
{
    GThread *info = (GThread *)info_;
    info->func(info->data);
    if (!info->joinable)
        free(info);
    return NULL;
}

GThread *fluid_g_thread_create(GThreadFunc func, void *data, gboolean joinable, GError **error)
{
    static GError error_container;
    g_return_val_if_fail(func != NULL, NULL);

    GThread *info = (GThread *)malloc(sizeof(GThread));
    if (info == NULL)
        return NULL;
    info->func = func;
    info->data = data;
    info->joinable = joinable;

    int rc = pthread_create(&info->handle, NULL, g_thread_wrapper, info);
    if (error != NULL)
    {
        error_container.code = rc;
        error_container.message = rc ? strerror(rc) : NULL;
        *error = rc ? &error_container : NULL;
    }
    if (rc != 0)
    {
        free(info);
        return NULL;
    }
    if (!joinable)
        pthread_detach(info->handle);
    return info;
}

void fluid_g_thread_join(GThread *thread)
{
    if (thread != NULL && thread->joinable)
    {
        pthread_join(thread->handle, NULL);
        free(thread);
    }
}

GThread *fluid_g_thread_self(void)
{
    /* only used as an opaque thread id */
    return (GThread *)(uintptr_t)pthread_self();
}

#endif
