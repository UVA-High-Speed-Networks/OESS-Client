/**
 *   Copyright 2019, University Corporation for Atmospheric Research. All rights
 *   reserved. See the file COPYRIGHT in the top-level source-directory for
 *   licensing conditions.
 *
 * This file implements the the provider-independent `log.h` API. It provides
 * for accumulating log-messages into a thread-specific queue and the logging of
 * that queue at a single logging level.
 *
 * All publicly-available functions in this module are thread-safe.
 *
 * @file   log.c
 * @author Steven R. Emmerson
 *
 * REQUIREMENTS:
 *   - Can log to
 *     - System logging daemon (-l '')
 *     - Standard error stream (-l -) if it exists
 *     - File (-l _pathname_)
 *   - Default destination for log messages
 *     - To the standard error stream if it exists
 *     - Otherwise:
 *       - If backward-compatible: system logging daemon
 *       - If not backward-compatible: standard LDM log file
 *   - Pathname of standard LDM log file configurable at session time
 *   - Output format
 *     - If using system logging daemon: chosen by daemon
 *     - Otherwise:
 *       - Pattern: _time_ _process_ _priority_ _location_ _message_
 *         - _time_: <em>YYYYMMDD</em>T<em>hhmmss</em>.<em>uuuuuu</em>Z
 *         - _process_: _program_[_pid_]
 *         - _priority_: DEBUG | INFO | NOTE | WARN | ERROR
 *         - _location_: _file_:_func()_:_line_
 *       - Example: 20160113T150106.734013Z noaaportIngester[26398] NOTE process_prod.c:process_prod():216 SDUS58 PACR 062008 /pN0RABC inserted
 *   - Enable log file rotation
 */
#include <config.h>

#undef NDEBUG
#include "log.h"
#include "StrBuf.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>   /* NULL */
#include <stdio.h>    /* vsnprintf(), snprintf() */
#include <stdlib.h>   /* malloc(), free(), abort() */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef _XOPEN_NAME_MAX
    #define _XOPEN_NAME_MAX 255 // Not always defined
#endif

#ifndef _XOPEN_PATH_MAX
    #define _XOPEN_PATH_MAX 1024 // Not always defined
#endif

/******************************************************************************
 * Private API:
 ******************************************************************************/

/**
 * A queue of log messages.
 */
typedef struct msg_queue {
    Message*    first;
    Message*    last;           /* NULL => empty queue */
} msg_queue_t;

/**
 * Whether or not this module is initialized.
 */
static volatile sig_atomic_t isInitialized = false;
/**
 * Key for the thread-specific queue of log messages.
 */
static pthread_key_t         queueKey;
/**
 * The thread identifier of the thread on which `log_init()` was called.
 */
static pthread_t             init_thread;
/**
 * Whether or not to avoid using the standard error stream.
 */
static bool                  avoid_stderr;
/**
 * Whether this module needs to be refreshed.
 */
static bool                  refresh_needed;
/**
 * The mutex that makes this module thread-safe.
 */
static pthread_mutex_t       log_mutex;

/**
 * Initializes a location structure from another location structure.
 *
 * @param[out] dest    The location to be initialized.
 * @param[in]  src     The location whose values are to be used for
 *                     initialization.
 * @threadsafety       Safe
 * @asyncsignalsafety  Safe
 */
static void loc_init(
        log_loc_t* const restrict       dest,
        const log_loc_t* const restrict src)
{
    dest->file = src->file; // `__FILE__` is persistent

    char*       d = dest->func_buf;
    const char* s = src->func;

    while (*s && d < dest->func_buf + sizeof(dest->func_buf) - 1)
        *d++ = *s++;

    *d = 0;
    dest->func = dest->func_buf;
    dest->line = src->line;
}

/**
 * Returns a new message structure.
 *
 * @retval    NULL     Failure. `logl_internal()` called.
 * @return             New message structure
 * @threadsafety       Safe
 * @asyncsignalsafety  UnSafe
 */
static Message* msg_new(void)
{
    Message* msg = malloc(sizeof(Message));

    if (msg == NULL) {
        logl_internal(LOG_LEVEL_ERROR, "malloc() failure: size=%zu",
                sizeof(Message));
    }
    else {
        #define LOG_DEFAULT_STRING_SIZE     256
        char*   string = malloc(LOG_DEFAULT_STRING_SIZE);

        if (NULL == string) {
            logl_internal(LOG_LEVEL_ERROR, "malloc() failure: size=%u",
                    LOG_DEFAULT_STRING_SIZE);
            free(msg);
            msg = NULL;
        }
        else {
            *string = 0;
            msg->string = string;
            msg->size = LOG_DEFAULT_STRING_SIZE;
            msg->next = NULL;
        }
    } // `msg` allocated

    return msg;
}

/**
 * Prints a message into a message-queue entry.
 *
 * @param[in] msg        The message entry.
 * @param[in] fmt        The message format.
 * @param[in] args       The arguments to be formatted.
 * @retval    0          Success. The message has been written into `*msg`.
 * @retval    EINVAL     `fmt` or `args` is `NULL`. Error message logged.
 * @retval    EINVAL     There are insufficient arguments. Error message logged.
 * @retval    EILSEQ     A wide-character code that doesn't correspond to a
 *                       valid character has been detected. Error message logged.
 * @retval    ENOMEM     Out-of-memory. Error message logged.
 * @retval    EOVERFLOW  The length of the message is greater than {INT_MAX}.
 *                       Error message logged.
 * @threadsafety         Safe
 * @asyncsignalsafety    Unsafe
 */
static int msg_format(
        Message* const restrict    msg,
        const char* const restrict fmt,
        va_list                    args)
{
    va_list argsCopy;

    va_copy(argsCopy, args);
        int nbytes = vsnprintf(msg->string, msg->size, fmt, args);
        int status;

        if (msg->size > nbytes) {
            status = 0;
        }
        else if (0 > nbytes) {
            // EINTR, EILSEQ, ENOMEM, or EOVERFLOW
            logl_internal(LOG_LEVEL_ERROR, "vsnprintf() failure: "
                    "fmt=\"%s\", errno=\"%s\"", fmt, strerror(errno));
        }
        else {
            // The buffer is too small for the message. Expand it.
            size_t  size = nbytes + 1;
            char*   string = malloc(size);

            if (NULL == string) {
                status = errno;
                logl_internal(LOG_LEVEL_ERROR, "malloc() failure: size=%zu",
                        size);
            }
            else {
                free(msg->string);
                msg->string = string;
                msg->size = size;
                (void)vsnprintf(msg->string, msg->size, fmt, argsCopy);
                status = 0;
            }
        }                           /* buffer is too small */
    va_end(argsCopy);

    return status;
}

/**
 * Creates the key for accessing the thread-specific queue of messages.
 *
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static void queue_create_key(void)
{
    int status = pthread_key_create(&queueKey, NULL);

    if (status != 0) {
        logl_internal(LOG_LEVEL_ERROR, "pthread_key_create() failure: "
                "errno=\"%s\"", strerror(status));
        abort();
    }
}

/**
 * Returns the current thread's message-queue.
 *
 * This function is thread-safe. On entry, this module's lock shall be
 * unlocked.
 *
 * @return             The message-queue of the current thread or NULL if the
 *                     message-queue doesn't exist and couldn't be created.
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static msg_queue_t* queue_get(void)
{
    /**
     * Whether or not the thread-specific queue key has been created.
     */
    static pthread_once_t key_creation_control = PTHREAD_ONCE_INIT;

    (void)pthread_once(&key_creation_control, queue_create_key);

    msg_queue_t* queue = pthread_getspecific(queueKey);

    if (NULL == queue) {
        queue = malloc(sizeof(msg_queue_t));

        if (NULL == queue) {
            logl_internal(LOG_LEVEL_ERROR, "malloc() failure: size=%zu",
                    sizeof(msg_queue_t));
        }
        else {
            int status = pthread_setspecific(queueKey, queue);

            if (status != 0) {
                logl_internal(LOG_LEVEL_ERROR, "pthread_setspecific() failure: "
                        "errno=\"%s\"", strerror(status));
                free(queue);
                queue = NULL;
            }
            else {
                queue->first = NULL;
                queue->last = NULL;
            }
        }
    }

    return queue;
}

/**
 * Indicates if a given message queue is empty.
 *
 * @param[in] queue    The message queue.
 * @retval    true     Iff `queue == NULL` or the queue is empty
 * @threadsafety       Safe
 * @asyncsignalsafety  Safe
 */
static bool queue_is_empty(
        msg_queue_t* const queue)
{
    return queue == NULL || queue->last == NULL;
}

/**
 * Indicates if the message queue of the current thread is empty.
 *
 * @retval true        Iff the queue is empty
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static bool logl_is_queue_empty()
{
    msg_queue_t* const queue = queue_get();
    const bool         is_empty = queue_is_empty(queue);

    return is_empty;
}

/**
 * Returns the next unused entry in a message-queue. Creates it if necessary.
 *
 * @param[in] queue    The message-queue.
 * @param[in] entry    The next unused entry.
 * @retval    0        Success. `*entry` is set.
 * @retval    ENOMEM   Out-of-memory. Error message logged.
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static int queue_getNextEntry(
        msg_queue_t* const restrict queue,
        Message** const restrict    entry)
{
    int      status;
    Message* msg = (NULL == queue->last) ? queue->first : queue->last->next;

    if (msg != NULL) {
        *entry = msg;
        status = 0;
    }
    else {
        msg = msg_new();

        if (msg == NULL) {
            status = ENOMEM;
        }
        else {
            if (NULL == queue->first)
                queue->first = msg;  /* very first message */

            if (NULL != queue->last)
                queue->last->next = msg;

            *entry = msg;
            status = 0;
        } // `msg` allocated
    } // need new message structure

    return status;
}

/**
 * Clears the accumulated log-messages of the current thread.
 *
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static void queue_clear()
{
    msg_queue_t*   queue = queue_get();

    if (NULL != queue)
        queue->last = NULL;
}

/**
 * Destroys a queue of log-messages.
 *
 * @param[in] queue    The queue of messages.
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static void
queue_fini(
        msg_queue_t* const queue)
{
    Message* msg;
    Message* next;

    for (msg = queue->first; msg; msg = next) {
        next = msg->next;
        free(msg->string);
        free(msg);
    }
}

#ifdef NDEBUG
    #define LOGL_ASSERT(expr)
#else
    #define LOGL_ASSERT(expr) do { \
        if (!(expr)) { \
            logl_internal(LOG_LEVEL_ERROR, "Assertion failure: %s", #expr); \
            abort(); \
        } \
    } while (false)
#endif

void logl_lock(void)
{
    int status = pthread_mutex_lock(&log_mutex);

    LOGL_ASSERT(status == 0);
}

void logl_unlock(void)
{
    int status = pthread_mutex_unlock(&log_mutex);

    LOGL_ASSERT(status == 0);
}

/**
 * Asserts that the current thread has acquired this module's lock.
 */
static void assertLocked(void)
{
    LOGL_ASSERT(pthread_mutex_trylock(&log_mutex));
}

/**
 * Returns the default destination for log messages, which depends on whether or
 * not log_avoid_stderr() has been called. If it hasn't been called, then the
 * default destination will be the standard error stream; otherwise, the default
 * destination will be that given by log_get_default_daemon_destination().
 *
 * @pre                Module is locked
 * @retval ""          Log to the system logging daemon
 * @retval "-"         Log to the standard error stream
 * @return             The pathname of the log file
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static const char* get_default_destination(void)
{
    assertLocked();

    return avoid_stderr
            ? log_get_default_daemon_destination()
            : "-";
}

/**
 * Registers functions to call when a new process is created with `fork()`.
 *
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static void register_atfork_funcs(void)
{
    int status = pthread_atfork(logl_lock, logl_unlock, logl_unlock);

    LOGL_ASSERT(status == 0);
}

/**
 * Initializes this logging module. Called by `log_init()`.
 *
 * @retval     0       Success
 * @retval     ENOMEM  Out-of-memory.
 * @retval     EINVAL  `log_mutex` is invalid.
 * @retval     EPERM   Module is already initialized.
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static int init(void)
{
    int status;

    if (isInitialized) {
        logl_internal(LOG_LEVEL_ERROR, "Logging module already initialized");
        status = EPERM;
    }
    else {
        /*
         * The following mutex isn't error-checking or recursive because a
         * glibc-created child process can't release such mutexes because the
         * thread in the child isn't the same as the thread in the parent. See
         * <https://stackoverflow.com/questions/5473368/pthread-atfork-locking-idiom-broken>.
         * As a consequence, failure to lock or unlock the mutex must not
         * result in a call to a logging function that attempts to lock or
         * unlock the mutex.
         */
        status = pthread_mutex_init(&log_mutex, NULL);

        if (status) {
            logl_internal(LOG_LEVEL_ERROR, "Couldn't initialize mutex: %s",
                    strerror(status));
        }
        else {
            static pthread_once_t atfork_control = PTHREAD_ONCE_INIT;

            status = pthread_once(&atfork_control, register_atfork_funcs);

            if (status) {
                logl_internal(LOG_LEVEL_ERROR, "pthread_once() failure: %s",
                        strerror(status));
                (void)pthread_mutex_destroy(&log_mutex);
            }
        } // `log_mutex` initialized
    } // Module isn't initialized

    return status;
}

/**
 * Indicates if a message at a given logging level would be logged.
 *
 * @param[in] level    The logging level
 * @retval    true     iff a message at level `level` would be logged
 * @threadsafety       Safe
 * @asyncsignalsafety  Safe
 */
static bool is_level_enabled(
        const log_level_t level)
{
    assertLocked();

    return logl_vet_level(level) && level >= log_level;
}

/**
 * Refreshes the logging module. If logging is to the system logging daemon,
 * then it will continue to be. If logging is to a file, then the file is closed
 * and re-opened; thus enabling log file rotation. If logging is to the standard
 * error stream, then it will continue to be if log_avoid_stderr() hasn't been
 * called; otherwise, logging will be to the provider default. Should be called
 * after log_init().
 *
 * @pre                Module is locked
 * @retval  0          Success
 * @retval -1          Failure
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static inline int refresh_if_necessary(void)
{
    assertLocked();

    int status = 0;

    if (refresh_needed) {
        status = logi_reinit();
        refresh_needed = 0;
    }

    return status;
}

/**
 * Logs the currently-accumulated log-messages of the current thread and resets
 * the message-queue for the current thread.
 *
 * @param[in] level    The level at which to log the messages. One of
 *                     LOG_LEVEL_ERROR, LOG_LEVEL_WARNING, LOG_LEVEL_NOTICE,
 *                     LOG_LEVEL_INFO, or LOG_LEVEL_DEBUG; otherwise, the
 *                     behavior is undefined.
 * @retval    0        Success
 * @retval    -1       Error
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static int flush(
    const log_level_t level)
{
    int          status = 0; // Success
    msg_queue_t* queue = queue_get();

    if (NULL != queue && NULL != queue->last) {
        logl_lock();

        if (!is_level_enabled(level)) {
            logl_unlock();
        }
        else {
            (void)refresh_if_necessary();
            logl_unlock();

            for (const Message* msg = queue->first; NULL != msg;
                    msg = msg->next) {
                status = logi_log(level, &msg->loc, msg->string);

                if (status)
                    break;

                if (msg == queue->last)
                    break;
            } // Message loop

            status = logi_flush();
        } // Messages should be printed

        queue_clear();
    } // Have messages

    return status;
}

/******************************************************************************
 * Package-private API:
 ******************************************************************************/

/**
 *  Logging level.
 */
log_level_t log_level = LOG_LEVEL_NOTICE;

/**
 * The mapping from logging levels to system logging daemon priorities:
 */
static int log_syslog_priorities[] = {
        LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING, LOG_ERR
};

int logl_level_to_priority(
        const log_level_t level)
{
    return logl_vet_level(level) ? log_syslog_priorities[level] : LOG_ERR;
}

const char* logl_basename(
        const char* const pathname)
{
    const char* const cp = strrchr(pathname, '/');
    return cp ? cp + 1 : pathname;
}

/**
 * Tries to return a formatted variadic message.
 * @param[out] msg          Formatted message. Caller should free if the
 *                          returned number of bytes is less than the estimated
 *                          number of bytes.
 * @param[in]  nbytesGuess  Estimated size of message in bytes -- *including*
 *                          the terminating NUL
 * @param[in]  format       Message format
 * @param[in]  args         Optional argument of format
 * @retval     -1           Out-of-memory. `logl_internal()` called.
 * @return                  Number of bytes necessary to contain the formatted
 *                          message -- *excluding* the terminating NUL. If
 *                          greater than or equal to the estimated number of
 *                          bytes, then `*msg` isn't set.
 * @threadsafety            Safe
 * @asyncsignalsafety       Unsafe
 */
static ssize_t tryFormatingMsg(
        char** const      msg,
        const ssize_t     nbytesGuess,
        const char* const format,
        va_list           args)
{
    ssize_t nbytes;
    char*   buf = malloc(nbytesGuess);

    if (buf == NULL) {
        logl_internal(LOG_LEVEL_ERROR, "Couldn't allocate %ld-byte message "
                "buffer", (long)nbytesGuess);
        nbytes = -1;
    }
    else {
        nbytes = vsnprintf(buf, nbytesGuess, format, args);
        if (nbytes >= nbytesGuess) {
            free(buf);
        }
        else {
            *msg = buf;
        }
    }
    return nbytes;
}

/**
 * Returns a a formated variadic message.
 * @param[in] format   Message format
 * @param[in] args     Optional format arguments
 * @retval    NULL     Out-of-memory. `logl_internal()` called.
 * @return             Allocated string of formatted message. Caller should free
 *                     when it's no longer needed.
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static char* formatMsg(
        const char* format,
        va_list     args)
{
    va_list argsCopy;

    va_copy(argsCopy, args);
        char*   msg = NULL;
        ssize_t nbytes = tryFormatingMsg(&msg, 256, format, args);

        if (nbytes >= 256)
            (void)tryFormatingMsg(&msg, nbytes+1, format, argsCopy);
    va_end(argsCopy);

    return msg;
}

int logl_vlog_1(
        const log_loc_t* const  loc,
        const log_level_t       level,
        const char* const       format,
        va_list                 args)
{
    int status;

    logl_lock();

    if (!is_level_enabled(level)) {
        logl_unlock();
        status = 0; // Success
    }
    else {
        char* msg = formatMsg(format, args);

        if (msg == NULL) {
            logl_unlock();
            status = -1;
        }
        else {
            (void)refresh_if_necessary();
            logl_unlock();

            status = logi_log(level, loc, msg);

            if (status == 0)
                status = logi_flush();

            free(msg);
        } // Have message
    } // Message should be logged

    return status;
}

int logl_vadd(
        const log_loc_t* const restrict loc,
        const char* const restrict      fmt,
        va_list                         args)
{
    int status;

    if (NULL == fmt) {
        logl_internal(LOG_LEVEL_ERROR, "NULL argument");
        status = EINVAL;
    }
    else {
        msg_queue_t* queue = queue_get();
        if (NULL == queue) {
            status = ENOMEM;
        }
        else {
            Message* msg;
            status = queue_getNextEntry(queue, &msg); // `queue != NULL`
            if (status == 0) {
                loc_init(&msg->loc, loc);
                status = msg_format(msg, fmt, args);
                if (status == 0)
                    queue->last = msg;
            } // have a message structure
        } // message-queue isn't NULL
    } // arguments aren't NULL

    return status;
}

int logl_add(
        const log_loc_t* const restrict loc,
        const char* const restrict      fmt,
                                        ...)
{
    va_list  args;

    va_start(args, fmt);
        int status = logl_vadd(loc, fmt, args);
    va_end(args);

    return status;
}

int logl_add_errno(
        const log_loc_t* const loc,
        const int              errnum,
        const char* const      fmt,
                               ...)
{
    int status = logl_add(loc, "%s", strerror(errnum));

    if (status == 0 && fmt && *fmt) {
        va_list     args;

        va_start(args, fmt);
            status = logl_vadd(loc, fmt, args);
        va_end(args);
    }

    return status;
}

void* logl_malloc(
        const char* const restrict file,
        const char* const restrict func,
        const int                  line,
        const size_t               nbytes,
        const char* const          msg)
{
    void* obj = malloc(nbytes);

    if (obj == NULL) {
        log_loc_t loc = {file, func, line};
        logl_add(&loc, "Couldn't allocate %lu bytes for %s", nbytes, msg);
    }

    return obj;
}

void* logl_realloc(
        const char* const file,
        const char* const func,
        const int         line,
        void*             buf,
        const size_t      nbytes,
        const char* const msg)
{
    void* obj = realloc(buf, nbytes);

    if (obj == NULL) {
        log_loc_t loc = {file, func, line};
        logl_add(&loc, "Couldn't re-allocate %lu bytes for %s", nbytes, msg);
    }

    return obj;
}

int logl_vlog_q(
        const log_loc_t* const restrict loc,
        const log_level_t               level,
        const char* const restrict      format,
        va_list                         args)
{
    int status;

    if (format && *format) {
#if 1
        logl_vadd(loc, format, args);
    }
    return flush(level);
#else
        StrBuf* sb = sb_get();
        if (sb == NULL) {
            logl_internal(LOG_LEVEL_ERROR,
                    "Couldn't get thread-specific string-buffer");
        }
        else if (sbPrintV(sb, format, args) == NULL) {
            logl_internal(LOG_LEVEL_ERROR,
                    "Couldn't format message into string-buffer");
        }
        else {
            logi_log(level, loc, sbString(sb));
        }
    }
#endif
}

int logl_log_1(
        const log_loc_t* const restrict loc,
        const log_level_t               level,
        const char* const restrict      format,
                                        ...)
{
    va_list args;

    va_start(args, format);
        int status = logl_vlog_1(loc, level, format, args);
    va_end(args);

    return status;
}

int logl_errno_1(
        const log_loc_t* const     loc,
        const int                  errnum,
        const char* const restrict fmt,
                                   ...)
{
    va_list args;

    va_start(args, fmt);
        int status = logl_log_1(loc, LOG_LEVEL_ERROR, "%s", strerror(errnum));

        if (status == 0)
            status = logl_vlog_1(loc, LOG_LEVEL_ERROR, fmt, args);
    va_end(args);

    return status;
}

int logl_log_q(
        const log_loc_t* const restrict loc,
        const log_level_t               level,
        const char* const restrict      format,
                                        ...)
{
    va_list args;

    va_start(args, format);
        int status = logl_vlog_q(loc, level, format, args);
    va_end(args);

    return status;
}

int logl_errno_q(
        const log_loc_t* const     loc,
        const int                  errnum,
        const char* const restrict fmt,
                                   ...)
{
    va_list args;

    va_start(args, fmt);
        logl_add(loc, "%s", strerror(errnum));
        int status = logl_vlog_q(loc, LOG_LEVEL_ERROR, fmt, args);
    va_end(args);

    return status;
}

int logl_flush(
        const log_loc_t* const loc,
        const log_level_t      level)
{
    int status = 0; // Success

    if (!logl_is_queue_empty()) {
        /*
         * The following message is added so that the location of the call to
         * log_flush() is logged in case the call needs to be adjusted.
        logl_add(loc, "Log messages flushed");
         */
        status = flush(level);
    }

    return status;
}

/**
 * Frees the log-message resources of the current thread. Should only be called
 * when no more logging by the current thread will occur.
 *
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static void logl_free(
        const log_loc_t* const loc)
{
    msg_queue_t* queue = queue_get();

    if (!queue_is_empty(queue)) {
        logl_log_q(loc, LOG_LEVEL_WARNING,
                "logl_free() called with the above messages still in the "
                "message-queue");
    }

    if (queue) {
        queue_fini(queue);
        free(queue);
        (void)pthread_setspecific(queueKey, NULL);
    }
}

/**
 * Finalizes the logging module. Frees all thread-specific resources. Frees all
 * thread-independent resources if the current thread is the one on which
 * log_init() was called.
 *
 * @retval -1          Failure
 * @retval  0          Success
 * @threadsafety       Safe
 * @asyncsignalsafety  Unsafe
 */
static int logl_fini(
        const log_loc_t* const loc)
{
    int status;

    if (!isInitialized) {
        // Can't log an error message because not initialized
        status = -1;
    }
    else {
        logl_free(loc);
        if (!pthread_equal(init_thread, pthread_self())) {
            status = 0;
        }
        else {
            status = logi_fini();
        }
    }
    return status ? -1 : 0;
}

/******************************************************************************
 * Public API:
 ******************************************************************************/

bool log_is_stderr_useful(void)
{
    static struct stat dev_null_stat;
    static bool        initialized = false;

    if (!initialized) {
        (void)stat("/dev/null", &dev_null_stat); // Can't fail
        initialized = true;
    }

    struct stat stderr_stat;

    return (fstat(STDERR_FILENO, &stderr_stat) == 0) &&
        ((stderr_stat.st_ino != dev_null_stat.st_ino) ||
                (stderr_stat.st_dev != dev_null_stat.st_dev));
}

int log_init(
        const char* const id)
{
    int status = init();

    if (status == 0) {
        logl_lock();
            log_level = LOG_LEVEL_NOTICE;
            status = logi_init(id);

            if (status == 0) {
                init_thread = pthread_self();
                // `avoid_stderr` must be set before `get_default_destination()`
                avoid_stderr = !log_is_stderr_useful();
                status = logi_set_destination(get_default_destination());
                isInitialized = status == 0;
            }
        logl_unlock();
    }

    return status == 0 ? 0 : -1;
}

void log_avoid_stderr(void)
{
    logl_lock();
        avoid_stderr = true;

        // Don't change if unnecessary
        if (LOG_IS_STDERR_SPEC(logi_get_destination()))
            (void)logi_set_destination(log_get_default_daemon_destination());
    logl_unlock();
}

void log_refresh(void)
{
    logl_lock();
        refresh_needed = 1;
    logl_unlock();
}

int log_set_id(
        const char* const id)
{
    return (id == NULL)
        ? -1
        : logi_set_id(id);
}

int log_set_upstream_id(
        const char* const hostId,
        const bool        isFeeder)
{
    int status;

    if (hostId == NULL) {
        status = -1;
    }
    else {
        char id[_POSIX_HOST_NAME_MAX + 6 + 1]; // hostname + "(type)" + 0

        (void)snprintf(id, sizeof(id), "%s(%s)", hostId,
                isFeeder ? "feed" : "noti");
        id[sizeof(id)-1] = 0;

        status = logi_set_id(id);
    }

    return status;
}

const char* log_get_default_destination(void)
{
    logl_lock();
        const char* dest = get_default_destination();
    logl_unlock();

    return dest;
}

int log_set_destination(
        const char* const dest)
{
    return (dest == NULL)
            ? -1
            : logi_set_destination(dest);
}

const char* log_get_destination(void)
{
    return logi_get_destination();
}

int log_set_level(
        const log_level_t level)
{
    int status;

    if (!logl_vet_level(level)) {
        status = -1;
    }
    else {
        logl_lock();
            log_level = level;
        logl_unlock();

        status = 0;
    }

    return status;
}

void log_roll_level(void)
{
    logl_lock();
        log_level = (log_level == LOG_LEVEL_DEBUG)
                ? LOG_LEVEL_ERROR
                : log_level - 1;
    logl_unlock();
}

log_level_t log_get_level(void)
{
    logl_lock(); // For visibility of changes
        log_level_t level = log_level;
    logl_unlock();

    return level;
}

bool log_is_level_enabled(
        const log_level_t level)
{
    logl_lock();
        bool enabled = is_level_enabled(level);
    logl_unlock();

    return enabled;
}

void
log_clear(void)
{
    queue_clear();
}

void
log_free_located(
        const log_loc_t* const loc)
{
    logl_free(loc);
}

int log_fini_located(
        const log_loc_t* const loc)
{
    int status = logl_fini(loc);

    if (status == 0) {
        status = pthread_mutex_destroy(&log_mutex);

        if (status == 0)
            isInitialized = false;
    }

    return status ? -1 : 0;
}
