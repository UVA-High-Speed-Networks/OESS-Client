/**
 * @mainpage LDM Logging
 *
 * Copyright 2016 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 * @author: Steven R. Emmerson
 *
 * @section contents Table of Contents
 * - \ref introduction
 * - \ref example
 *
 * @section introduction Introduction
 * This module is the logging system for the LDM. It comprises a single API with
 * two implementations: one using a simple implementation and the other using
 * the original `ulog` module that came with the LDM (that module is still part
 * of the LDM library for backward compatibility with user-developed code). By
 * default the simple implementation is used. The `ulog` implementation will be
 * used if the option `--with-ulog` is given to the `configure` script.
 *
 * This module manages a FIFO queue of log messages for each thread in a
 * process. `log_add*` function add to the queue. At some point, one of the
 * following should occur:
 *   - A final message added and the accumulated messages emitted by
 *     `log_error()`, for example;
 *   - The accumulated messages emitted by one of the `log_flush_*` functions;
 *     or
 *   - The queue cleared by `log_clear()`.
 *
 * By default, emitted messages are written to
 *   - If the process is a daemon (i.e., doesn't have a controlling terminal)
 *     - The LDM log file for the simple implementation; or
 *     - The system logging daemon for the `ulog` implementation.
 *   - The standard error stream if the process is not a daemon.
 * (Note that the LDM server, `ldmd`, daemonizes itself by default. It is the
 * only program that does.)
 *
 * The default destination for log messages can usually be overridden by a
 * command-line option.
 *
 * Besides managing thread-specific queues of log messages, this module also
 * registers a handler for the `HUP` signal. If log messages are being written
 * to a regular file (e.g., the LDM log file), then upon receipt of the signal,
 * the module will refresh (i.e., close and re-open) its connection to the file.
 * This allows the log files to be rotated and purged by an external process so
 * that the disk partition doesn't become full.
 *
 * @section example Example
 * Here's a contrived example:
 *
 * @code{.c}
 *     #include <log.h>
 *     #include <errno.h>
 *     #include <unistd.h>
 *
 *     static int system_failure()
 *     {
 *         (void)close(-1); // Guaranteed failure
 *         log_add_syserr("close() failure"); // Uses `errno`; adds to queue
 *         return -1;
 *     }
 *
 *     static int func()
 *     {
 *         int status = system_failure();
 *         if (status)
 *             log_add("system_failure() returned %d", status); // adds to queue
 *         return status;
 *     }
 *
 *     int main(int ac, char* av)
 *     {
 *         ...
 *         log_init(av[0]); // Necessary
 *         ...
 *         while ((int c = getopt(ac, av, "l:vx") != EOF) {
 *             extern char *optarg;
 *             switch (c) {
 *                 case 'l':
 *                      (void)log_set_output(optarg);
 *                      break;
 *                 case 'v':
 *                      (void)log_set_level(LOG_LEVEL_INFO);
 *                      break;
 *                 case 'x':
 *                      (void)log_set_level(LOG_LEVEL_DEBUG);
 *                      break;
 *                 ...
 *             }
 *         }
 *         ...
 *         if (func()) {
 *             if (log_is_enabled_info)
 *                 // Adds to queue, prints queue at INFO level, and clears queue
 *                 log_info("func() failure: reason = %s", expensive_func());
 *         }
 *         if (func()) {
 *             // Adds to queue, prints queue at ERROR level, and clears queue
 *             log_error("func() failure: reason = %s", cheap_func());
 *         }
 *         ...
 *         (void)log_fini(); // Good form
 *         return 0;
 *     }
 * @endcode
 */
