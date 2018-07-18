/**
 * This file defines an executor of asynchronous tasks.
 *
 * Copyright 2018, University Corporation for Atmospheric Research
 * All rights reserved. See file COPYRIGHT in the top-level source-directory for
 * copying and redistribution conditions.
 * 
 *        File: Executor.c
 *  Created on: May 7, 2018
 *      Author: Steven R. Emmerson
 */
#include "config.h"

#include "Executor.h"
#include "log.h"
#include "Thread.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>

/******************************************************************************
 * Job to be executed:
 ******************************************************************************/

typedef enum {
    JOB_INITIALIZED,
    JOB_EXECUTING,
    JOB_COMPLETED
} JobState;

typedef struct job {
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    pthread_t        thread;
    /// Object to be executed
    void*            obj;
    /// Function to execute object
    int            (*run)(void* obj, void** result);
    /// Function to halt execution of object
    int            (*halt)(void* obj, pthread_t thread);
    /// Future of task submitted to `executor_submitFuture()`
    Future*          future;
    struct executor* executor;   ///< Associated execution service
    struct job*      prev;       ///< Previous job in list
    struct job*      next;       ///< Next job in list
    JobState         state;      ///< State of job
    bool             canceled;   ///< Job was canceled?
} Job;

// Forward declaration
typedef struct executor Executor;

inline static void
job_lock(Job* const job)
{
    int status = pthread_mutex_lock(&job->mutex);
    log_assert(status == 0);
}

inline static void
job_unlock(Job* const job)
{
    int status = pthread_mutex_unlock(&job->mutex);
    log_assert(status == 0);
}

inline static void
job_assertLocked(Job* const job)
{
#ifndef NDEBUG
    int status = pthread_mutex_trylock(&job->mutex);
    log_assert(status != 0);
#endif
}

static int
job_defaultHaltFunc(
        void*     obj,
        pthread_t thread)
{
    /*
     * The default mechanism for stopping an asynchronous task should work even
     * if the task's thread is currently in `poll()`. Possible mechanisms
     * include using
     *   - `pthread_kill()`: Possible if the RPC package that's included with
     *     the LDM is used rather than a standard RPC implementation (see call
     *     to `subscribe_7()` for more information). Requires that a signal
     *     handler be installed (one is in the top-level LDM). The same solution
     *     will also work to interrupt the `connect()` system call on the main
     *     thread.
     *   - `pthread_cancel()`: The resulting code is considerably more complex
     *     than for `pthread_kill()` and, consequently, more difficult to reason
     *     about.
     * The `pthread_kill()` solution was chosen.
     */
    int status = pthread_kill(thread, SIGTERM);

    if (status) {
        if (status == ESRCH) {
            status = 0; // Thread already terminated
        }
        else {
            log_add_errno(status, "Couldn't signal job's thread");
        }
    }

    return status;
}

/**
 * Allocates and initializes a new job.
 *
 * @param[in]  executor  Associated executor
 * @param[in]  future    Associated future
 * @param[in]  obj       Optional object to be executed or `NULL`
 * @param[in]  run       Function to execute
 * @param[in]  halt      Optional function to stop execution or `NULL`
 */
static Job*
job_new(
        Executor* const restrict executor,
        Future* const restrict   future,
        void* const              obj,
        int                    (*run)(void* obj, void** result),
        int                    (*halt)(void* obj, pthread_t thread))
{
    Job* job = log_malloc(sizeof(Job), "job");

    if (job) {
        job->obj = obj;
        job->run = run;
        job->halt = halt ? halt : job_defaultHaltFunc;
        job->executor = executor;
        job->prev = job->next = NULL;
        job->future = future;
        job->canceled = false;
        job->state = JOB_INITIALIZED;

        future_setJob(future, job);

        int status = mutex_init(&job->mutex, PTHREAD_MUTEX_ERRORCHECK, true);

        if (status) {
            log_add("Couldn't initialize job's mutex");
        }
        else {
            status = pthread_cond_init(&job->cond, NULL);

            if (status) {
                log_add_errno(status,
                        "Couldn't initialize job's condition-variable");
                mutex_destroy(&job->mutex);
            }
        } // `job->mutex` initialized

        if (status) {
            free(job);
            job = NULL;
        }
    } // `job` allocated

    return job;
}

/**
 * Frees a job. Doesn't free the job's future.
 *
 * @param[in,out] job     Job to be freed
 * @retval        0       Success
 */
static void
job_free(Job* const job)
{
    log_assert(job->state != JOB_EXECUTING);
    (void)pthread_cond_destroy(&job->cond);
    (void)pthread_mutex_destroy(&job->mutex);
    free(job);
}

/**
 * Sets the state of a job and signals the job's condition-variable.
 *
 * @pre                  Job is locked
 * @param[in,out] job    Job
 * @param[in]     state  New state
 * @post                 Job is locked
 */
static void
job_setState(
        Job* const     job,
        const JobState state)
{
    job_assertLocked(job);

    job->state = state;

    int status = pthread_cond_broadcast(&job->cond);
    log_assert(status == 0);
}

typedef struct executor Executor;

static void
executor_remove(
        Executor* const restrict executor,
        Job* const restrict      job);

static void
executor_lock(Executor* const executor);

static void
executor_unlock(Executor* const executor);

static void
job_run(Job* const job)
{
    void* result = NULL;
    int   status = 0;

    job_lock(job);
        if (!job->canceled) {
            job->thread = pthread_self();
            job->state = JOB_EXECUTING;

            job_unlock(job);
            // Potentially lengthy operation
            status = job->run(job->obj, &result);
            job_lock(job);

            job->state = JOB_COMPLETED;
        }
    job_unlock(job);

    Executor* const executor = job->executor;

    /*
     * The call to `executor_remove()` must happen before `future_setResult()`
     * to ensure that the job has been removed from the job-list before
     * `future_getResult()` returns
     */
    executor_remove(executor, job);

    // Allows `future_getResult()` to return
    future_setResult(job->future, status, result, job->canceled);
}

/**
 * Asynchronously cancels a job. If the job hasn't started executing, then it
 * never should.
 *
 * @param[in] job              Job
 * @retval    0                Success. Job completed or was successfully
 *                             canceled.
 * @retval    ENOTRECOVERABLE  State protected by mutex is not recoverable.
 *                             `log_add()` called.
 */
int
job_cancel(Job* const job)
{
    int status;

    job_lock(job);
        if (job->state == JOB_COMPLETED) {
            job_unlock(job);
            status = 0;
        }
        else {
            job->canceled = true;

            if (job->state == JOB_INITIALIZED) {
                job_unlock(job);
                status = 0;
            }
            else {
                log_assert(job->state == JOB_EXECUTING);

                job_unlock(job);

                status = job->halt(job->obj, job->thread);

                if (status)
                    log_error_1("Job's halt function returned %d", status);
            } // Job is executing
        } // Job hasn't completed

    return status;
}

/******************************************************************************
 * Thread-compatible but not thread-safe list of jobs. It allows the
 * execution-service to cancel all running jobs.
 ******************************************************************************/

typedef struct jobList {
    struct job*     head;
    struct job*     tail;
    size_t          size;
} JobList;

/**
 */
static JobList*
jobList_new()
{
    JobList* jobList = log_malloc(sizeof(JobList), "job list");

    if (jobList) {
        jobList->head = jobList->tail = NULL;
        jobList->size = 0;
    } // `jobList` allocated

    return jobList;
}

static void
jobList_free(JobList* const jobList)
{
    Job* job = jobList->head;

    while (job != NULL) {
        Job* next = job->next;
        job_free(job);
        job = next;
    }

    free(jobList);
}

static void
jobList_add(
        JobList* const restrict  jobList,
        Job* const restrict      job)
{
    job->prev = jobList->tail;

    if (jobList->head == NULL)
        jobList->head = job;
    if (jobList->tail)
        jobList->tail->next = job;

    jobList->tail = job;
    jobList->size++;
}

/**
 * Removes a specific job from a list of jobs. Doesn't delete the job.
 *
 * @param[in,out] jobList  List of jobs
 * @param[in]     job      Job to be removed
 * @threadsafety           Compatible but not safe
 */
static void
jobList_remove(
        JobList* const restrict jobList,
        Job* const restrict     job)
{
    Job* const prev = job->prev;
    Job* const next = job->next;

    if (prev) {
        prev->next = next;
    }
    else {
        jobList->head = next;
    }

    if (next) {
        next->prev = prev;
    }
    else {
        jobList->tail = prev;
    }

    jobList->size--;
}

static size_t
jobList_size(JobList* const jobList)
{
    return jobList->size;
}

static int
jobList_cancelAll(JobList* const jobList)
{
    int status = 0;

    for (Job* job = jobList->head; job != NULL; ) {
        Job* next = job->next;
        int  stat = future_cancel(job->future);

        if (stat) {
            log_add("Couldn't cancel job");
            if (status == 0)
                status = stat;
        }

        job = next;
    }

    return status;
}

/******************************************************************************
 * Thread-safe execution service:
 ******************************************************************************/

struct executor {
    pthread_mutex_t mutex;
    JobList*        jobList;
    void*           completer;
    int           (*afterCompletion)(void* completer, Future* future);
    bool            isShutdown;
};

static void
executor_lock(Executor* const executor)
{
    int status = pthread_mutex_lock(&executor->mutex);
    log_assert(status == 0);
}

static void
executor_unlock(Executor* const executor)
{
    int status = pthread_mutex_unlock(&executor->mutex);
    log_assert(status == 0);
}

static int
executor_init(Executor* const executor)
{
    int status;

    executor->isShutdown = false;
    executor->jobList = jobList_new();
    executor->afterCompletion = NULL;
    executor->completer = NULL;

    if (executor->jobList == NULL) {
        log_add("Couldn't create new job list");
        status = ENOMEM;
    }
    else {
        status = mutex_init(&executor->mutex, PTHREAD_MUTEX_ERRORCHECK, true);

        if (status)
            jobList_free(executor->jobList);
    } // `executor->jobList` allocated

    return status;
}

static void
executor_destroy(Executor* const executor)
{
    executor_lock(executor);
        jobList_free(executor->jobList);
    executor_unlock(executor);

    (void)mutex_destroy(&executor->mutex);
}

Executor*
executor_new(void)
{
    Executor* executor = log_malloc(sizeof(Executor), "executor");

    if (executor) {
        if (executor_init(executor)) {
            log_add("Couldn't initialize executor");
            free(executor);
            executor = NULL;
        }
    }

    return executor;
}

void
executor_free(Executor* const executor)
{
    executor_destroy(executor);
    free(executor);
}

void
executor_setAfterCompletion(
        Executor* const restrict executor,
        void* const restrict     completer,
        int                    (*afterCompletion)(
                                     void* restrict   completer,
                                     Future* restrict future))
{
    executor->completer = completer;
    executor->afterCompletion = afterCompletion;
}

static void*
executor_run(void* const arg)
{
    Job* const      job = (Job*)arg;
    Executor* const executor = job->executor;

    job_run(job);

    if (executor->afterCompletion &&
            executor->afterCompletion(executor->completer, job->future))
        log_add("Couldn't process job's future after completion");

    job_free(job); // Doesn't free the job's future

    log_free(); // Because end-of-thread

    return NULL;
}

/**
 * Submits a job for execution.
 *
 * @param[in] executor  Execution service
 * @param[in] job       Job to be submitted
 * @retval    0         Success
 * @retval    EPERM     Execution service is shut down
 */
static int
executor_submitJob(
        Executor* const restrict executor,
        Job* const restrict      job)
{
    int        status;

    executor_lock(executor);
        if (executor->isShutdown) {
            log_add("Executor is shut down");
            status = EPERM;
        }
        else {
            jobList_add(executor->jobList, job);

            pthread_t thread;
            status = pthread_create(&thread, NULL, executor_run, job);

            if (status) {
                log_add_errno(status, "Couldn't create job's thread");
                jobList_remove(executor->jobList, job);
            }
            else {
                /*
                 * The thread is detached because it can't be joined:
                 *   - `job_free()` can't join it because it's executing on
                 *     the same thread; and
                 *   - `future_getResult()` can't join it without precluding
                 *     an execution service implementation that uses a
                 *     thread pool.
                 */
                (void)pthread_detach(thread);
            } // Thread created
        } // Executor is not shut down
    executor_unlock(executor);

    return status;
}

Future*
executor_submit(
        Executor* const executor,
        void* const     obj,
        int           (*run)(void* obj, void** result),
        int           (*halt)(void* obj, pthread_t thread))
{
    Future* future = future_new();

    if (future == NULL) {
        log_add("Couldn't create new future");
    }
    else {
        int        status;
        Job* const job = job_new(executor, future, obj, run, halt);

        if (job == NULL) {
            log_add("Couldn't create new job");
            status = -1;
        }
        else {
            status = executor_submitJob(executor, job);

            if (status)
                job_free(job); // Doesn't free job's future
        } // `job` created

        if (status) {
            future_free(future); // Doesn't free future's job
            future = NULL;
        }
    } // `future` created

    return future;
}

/**
 * Removes a job from the list of executing jobs.
 *
 * @param[in,out] executor  Execution service
 * @param[in]     job       Job to be removed
 */
static void
executor_remove(
        Executor* const restrict executor,
        Job* const restrict      job)
{
    executor_lock(executor);
        jobList_remove(executor->jobList, job);
    executor_unlock(executor);
}

size_t
executor_size(Executor* const executor)
{
    executor_lock(executor);
        size_t size = jobList_size(executor->jobList);
    executor_unlock(executor);

    return size;
}

int
executor_shutdown(
        Executor* const executor,
        const bool      now)
{
    int status;

    executor_lock(executor);
        if (executor->isShutdown) {
            status = 0;
        }
        else {
            executor->isShutdown = true;
            status = now
                    ? jobList_cancelAll(executor->jobList)
                    : 0;
        }
    executor_unlock(executor);

    return status;
}
