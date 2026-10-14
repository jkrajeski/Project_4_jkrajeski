/*
 * restaurant_log.h — Log format specification for Restaurant Management Simulation
 *
 * DO NOT MODIFY THIS FILE.  The automated grader parses the log produced by
 * your simulation.  Any deviation from the format below will cause grading
 * failures.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * LOG FORMAT
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * The log is written to a plain-text file (default: simulation.log).
 *
 * HEADER LINE (first line, written once at simulation start):
 *
 *   TUID=<tuid> CUSTOMERS=<n> TABLES=<n> WAITERS=<n> CHEFS=<n>
 *
 *   Example:
 *   TUID=tuf12345 CUSTOMERS=20 TABLES=5 WAITERS=3 CHEFS=2
 *
 * EVENT LINES (one per logged event):
 *
 *   [T+SS.MMM] [ROLE    ] KEY=val KEY=val ... message
 *
 *   Field widths (fixed, space-padded):
 *     Timestamp : [T+SS.MMM]   — seconds and milliseconds since sim start,
 *                               always two digits for seconds, three for ms
 *     Role      : [ROLE    ]   — exactly 8 chars, left-aligned, space-padded
 *
 *   Valid ROLE tokens (before padding):
 *     CUSTOMER   WAITER   CHEF   SIM
 *
 *   Standard key=value tokens (appear before the free-text message):
 *     cid=<n>     customer id (0-based)
 *     wid=<n>     waiter id   (0-based)
 *     chid=<n>    chef id     (0-based)
 *     tid=<n>     table id    (0-based)
 *     state=<s>   customer state string (see below)
 *     item=<s>    menu item (no spaces)
 *     load=<n>    waiter current_customer_count after change
 *     qlen=<n>    queue length after enqueue/dequeue
 *
 *   Customer state strings (match CustomerState enum names exactly):
 *     ARRIVING  QUEUED  SEATED  ORDERING  WAITING_FOR_MEAL  EATING  LEAVING
 *
 * FOOTER LINE (last line, written once at simulation end):
 *
 *   [T+SS.MMM] [SIM     ] DONE total_served=<n>
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * REQUIRED EVENTS  (grader checks for all of these)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *  Event                  Emitted by        Example line
 *  ─────────────────────  ────────────────  ──────────────────────────────────
 *  Customer state change  customer_thread   [T+00.012] [CUSTOMER ] cid=3 state=QUEUED customer 3 joined waiting queue
 *  Waiter load change     waiter_thread     [T+00.034] [WAITER  ] wid=1 cid=3 load=2 waiter 1 seated customer 3
 *  Order enqueued         waiter_thread     [T+00.410] [WAITER  ] wid=1 cid=3 item=PASTA qlen=1 order placed
 *  Meal ready             chef_thread       [T+01.220] [CHEF    ] chid=0 cid=3 item=PASTA meal ready
 *  Simulation end         main/SIM thread   [T+08.441] [SIM     ] DONE total_served=20
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * EXAMPLE LOG EXCERPT
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *   TUID=tuf12345 CUSTOMERS=20 TABLES=5 WAITERS=3 CHEFS=2
 *   [T+00.001] [SIM     ] simulation started
 *   [T+00.003] [CUSTOMER] cid=0 state=ARRIVING customer 0 arrived
 *   [T+00.005] [CUSTOMER] cid=0 state=QUEUED customer 0 joined waiting queue
 *   [T+00.007] [WAITER  ] wid=0 cid=0 load=1 waiter 0 seated customer 0
 *   [T+00.008] [CUSTOMER] cid=0 state=SEATED tid=2 customer 0 seated at table 2
 *   [T+00.009] [CUSTOMER] cid=0 state=ORDERING customer 0 placed order
 *   [T+00.010] [WAITER  ] wid=0 cid=0 item=BURGER qlen=1 order placed
 *   [T+00.410] [CHEF    ] chid=0 cid=0 item=BURGER meal ready
 *   [T+00.411] [CUSTOMER] cid=0 state=EATING customer 0 received meal
 *   [T+02.896] [CUSTOMER] cid=0 state=LEAVING customer 0 leaving
 *   [T+02.897] [WAITER  ] wid=0 cid=0 load=0 waiter released customer
 *   [T+08.441] [SIM     ] DONE total_served=20
 *
 * ─────────────────────────────────────────────────────────────────────────────
 */

#ifndef RESTAURANT_LOG_H
#define RESTAURANT_LOG_H

#include <stdio.h>

/* ── Opaque log handle ────────────────────────────────────────────────────── */

typedef struct SimLog SimLog;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/*
 * log_init() — Open the log file and write the header line.
 *
 *   path  : path to the log file (e.g. "simulation.log")
 *   tuid  : your Temple University ID string (e.g. "tuf12345")
 *   customers, tables, waiters, chefs : simulation parameters
 *
 * Returns a heap-allocated SimLog on success, NULL on failure.
 * The returned handle must be passed to every subsequent log call.
 */
SimLog *log_init(const char *path,
                 const char *tuid,
                 int customers, int tables, int waiters, int chefs);

/*
 * log_destroy() — Flush and close the log file; free the handle.
 * After this call the pointer is invalid.
 */
void log_destroy(SimLog *log);

/* ── Structured event helpers ─────────────────────────────────────────────── */

/*
 * log_customer_state() — Record a customer state transition.
 *
 *   cid   : customer id
 *   state : new state string (e.g. "QUEUED", "SEATED", ...)
 *   tid   : table id, or -1 if not yet seated
 *   msg   : free-text message appended after key=value tokens
 *
 * Emits:  [T+SS.MMM] [CUSTOMER] cid=<cid> state=<state> [tid=<tid>] <msg>
 */
void log_customer_state(SimLog *log, int cid, const char *state,
                        int tid, const char *msg);

/*
 * log_waiter_load() — Record a waiter load change (seat or release).
 *
 *   wid  : waiter id
 *   cid  : customer id involved
 *   load : new value of waiter->current_customer_count
 *   msg  : free-text message
 *
 * Emits:  [T+SS.MMM] [WAITER  ] wid=<wid> cid=<cid> load=<load> <msg>
 */
void log_waiter_load(SimLog *log, int wid, int cid, int load,
                     const char *msg);

/*
 * log_order_queued() — Record an order being placed on the OrderQueue.
 *
 *   wid  : waiter id who took the order
 *   cid  : customer id
 *   item : menu item string (no spaces)
 *   qlen : queue length after enqueue
 *
 * Emits:  [T+SS.MMM] [WAITER  ] wid=<wid> cid=<cid> item=<item> qlen=<qlen> order placed
 */
void log_order_queued(SimLog *log, int wid, int cid,
                      const char *item, int qlen);

/*
 * log_meal_ready() — Record a chef completing a meal.
 *
 *   chid : chef id
 *   cid  : customer id
 *   item : menu item string
 *
 * Emits:  [T+SS.MMM] [CHEF    ] chid=<chid> cid=<cid> item=<item> meal ready
 */
void log_meal_ready(SimLog *log, int chid, int cid, const char *item);

/*
 * log_sim_end() — Write the simulation footer line.
 *
 *   total_served : final value of the customers_served counter
 *
 * Emits:  [T+SS.MMM] [SIM     ] DONE total_served=<n>
 */
void log_sim_end(SimLog *log, int total_served);

/* ── Low-level helper (use sparingly) ─────────────────────────────────────── */

/*
 * log_event() — Write a free-form event line.
 *
 *   role : role token before padding (e.g. "SIM", "CUSTOMER")
 *   fmt  : printf-style format string for the rest of the line
 *
 * The timestamp is computed automatically from sim start time.
 * This function is thread-safe.
 *
 * Prefer the structured helpers above; use log_event() only for
 * one-off messages (e.g. "simulation started").
 */
void log_event(SimLog *log, const char *role, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* RESTAURANT_LOG_H */
