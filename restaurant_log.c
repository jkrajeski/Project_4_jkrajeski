/*
 * restaurant_log.c — Thread-safe simulation log implementation.
 *
 * Compile alongside your simulation:
 *   gcc -o restaurant restaurant.c restaurant_log.c -lpthread
 *
 * Students: DO NOT MODIFY THIS FILE.
 */

#include "restaurant_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Internal structure ───────────────────────────────────────────────────── */

struct SimLog {
    FILE           *fp;          /* output file                  */
    pthread_mutex_t mutex;       /* serialises all writes        */
    struct timespec start;       /* CLOCK_MONOTONIC at log_init  */
};

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Milliseconds elapsed since sim start. */
static double elapsed_ms(const SimLog *log)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double secs = (double)(now.tv_sec  - log->start.tv_sec)
                + (double)(now.tv_nsec - log->start.tv_nsec) / 1e9;
    return secs * 1000.0;
}

/*
 * Write the fixed-width timestamp+role prefix, then hand off to the caller's
 * vfprintf.  Caller must hold log->mutex.
 *
 *   elapsed : milliseconds since sim start
 *   role    : role token (e.g. "CUSTOMER"), padded to 8 chars
 */
static void write_prefix(FILE *fp, double elapsed, const char *role)
{
    long   sec = (long)(elapsed / 1000.0);
    long   ms  = (long)(elapsed) % 1000;
    /* Clamp display to SS.MMM (two-digit seconds) */
    if (sec > 99) sec = 99;
    fprintf(fp, "[T+%02ld.%03ld] [%-8s] ", sec, ms, role);
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

SimLog *log_init(const char *path,
                 const char *tuid,
                 int customers, int tables, int waiters, int chefs)
{
    SimLog *log = calloc(1, sizeof *log);
    if (!log) return NULL;

    log->fp = fopen(path, "w");
    if (!log->fp) { free(log); return NULL; }

    if (pthread_mutex_init(&log->mutex, NULL) != 0) {
        fclose(log->fp); free(log); return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &log->start);

    /* Header line */
    fprintf(log->fp,
            "TUID=%s CUSTOMERS=%d TABLES=%d WAITERS=%d CHEFS=%d\n",
            tuid, customers, tables, waiters, chefs);

    log_event(log, "SIM", "simulation started");
    return log;
}

void log_destroy(SimLog *log)
{
    if (!log) return;
    fflush(log->fp);
    fclose(log->fp);
    pthread_mutex_destroy(&log->mutex);
    free(log);
}

/* ── Structured helpers ───────────────────────────────────────────────────── */

void log_customer_state(SimLog *log, int cid, const char *state,
                        int tid, const char *msg)
{
    pthread_mutex_lock(&log->mutex);
    double t = elapsed_ms(log);
    write_prefix(log->fp, t, "CUSTOMER");
    fprintf(log->fp, "cid=%d state=%s", cid, state);
    if (tid >= 0)
        fprintf(log->fp, " tid=%d", tid);
    fprintf(log->fp, " %s\n", msg);
    pthread_mutex_unlock(&log->mutex);
}

void log_waiter_load(SimLog *log, int wid, int cid, int load,
                     const char *msg)
{
    pthread_mutex_lock(&log->mutex);
    double t = elapsed_ms(log);
    write_prefix(log->fp, t, "WAITER");
    fprintf(log->fp, "wid=%d cid=%d load=%d %s\n", wid, cid, load, msg);
    pthread_mutex_unlock(&log->mutex);
}

void log_order_queued(SimLog *log, int wid, int cid,
                      const char *item, int qlen)
{
    pthread_mutex_lock(&log->mutex);
    double t = elapsed_ms(log);
    write_prefix(log->fp, t, "WAITER");
    fprintf(log->fp, "wid=%d cid=%d item=%s qlen=%d order placed\n",
            wid, cid, item, qlen);
    pthread_mutex_unlock(&log->mutex);
}

void log_meal_ready(SimLog *log, int chid, int cid, const char *item)
{
    pthread_mutex_lock(&log->mutex);
    double t = elapsed_ms(log);
    write_prefix(log->fp, t, "CHEF");
    fprintf(log->fp, "chid=%d cid=%d item=%s meal ready\n", chid, cid, item);
    pthread_mutex_unlock(&log->mutex);
}

void log_sim_end(SimLog *log, int total_served)
{
    pthread_mutex_lock(&log->mutex);
    double t = elapsed_ms(log);
    write_prefix(log->fp, t, "SIM");
    fprintf(log->fp, "DONE total_served=%d\n", total_served);
    pthread_mutex_unlock(&log->mutex);
}

/* ── Low-level helper ─────────────────────────────────────────────────────── */

void log_event(SimLog *log, const char *role, const char *fmt, ...)
{
    pthread_mutex_lock(&log->mutex);
    double t = elapsed_ms(log);
    write_prefix(log->fp, t, role);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log->fp, fmt, ap);
    va_end(ap);
    fputc('\n', log->fp);
    pthread_mutex_unlock(&log->mutex);
}
