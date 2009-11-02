/*
 *   See file ../COPYRIGHT for copying and redistribution conditions.
 */
#ifndef _LDM_SERVER_GLOBAL_H
#define _LDM_SERVER_GLOBAL_H

/*
 * Unless otherwise noted, globals are
 * declared (and initialized) in ldmd.c
 */

#include <rpc/rpc.h>  /* svc_req */
#include <signal.h>   /* sig_atomic_t */

#ifdef __cplusplus
extern "C" {
#endif

extern const char *conf_path;
extern volatile sig_atomic_t done;
extern const char *logfname;
extern const char *pqfname;
extern struct pqueue *pq;

/* timeout for rpc calls */
#ifndef DEFAULT_RPCTIMEO
#  define DEFAULT_RPCTIMEO  60
#endif
extern unsigned int rpctimeo;

/* time we sleep in pq_suspend() and before retrying connects */
extern unsigned int interval;

/*
 * Shut down a service connection that has been idle this long.
 * The keepalive timeout (for the other end) is
 * inactive_timeo/2 - 2 * interval;
 */
extern const int inactive_timeo;

/*
 * In requests,
 * we set 'from' to 'toffset' ago, and it may get
 * trimmed by  pq_clss_setfrom();
 */
#ifndef DEFAULT_OLDEST
#  define DEFAULT_OLDEST  3600
#endif
extern int max_latency;
extern int toffset;

extern void clr_pip_5(void);	        /* defined in svc5.c */
extern int read_conf(
    const char* const	conf_path,
    int			doSomething,    /* defined in parser.y */
    unsigned		defaultPort);

/*
 * Calls exit() if the "done" global variable is set; othewise, returns 1 so
 * that it can be easily used in programming loops.
 *
 * Arguments:
 *	status	Exit status for the call to exit().
 * Returns:
 *	1
 */
int
exitIfDone(
    const int	status);

#ifdef __cplusplus
}
#endif

#endif /*!_LDM_SERVER_GLOBAL_H*/
