/*
 * restaurant.h — Shared types and constants for the Restaurant Management Simulation.
 *
 * CIS 3207 — Project 4
 *
 * Students: include this header in restaurant.c.
 */

#ifndef RESTAURANT_H
#define RESTAURANT_H

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

/* ── Compile-time array size caps ─────────────────────────────────────────────
 *
 * Stack-allocated arrays (e.g. Customer customers[TOTAL_CUSTOMERS]) must not
 * exceed these bounds.  Runtime arguments that exceed a cap produce an error.
 */
#define TOTAL_CUSTOMERS          20
#define NUM_WAITERS               3
#define NUM_CHEFS                 2

/* ── Simulation tuning ────────────────────────────────────────────────────────*/

#define MAX_CUSTOMERS_PER_WAITER  4    /* waiter blocks when load reaches this */

/* Timing ranges (milliseconds) — nanosleep is used; values * 1 000 000 = ns */
#define ARRIVE_MIN_MS            10
#define ARRIVE_MAX_MS           200
#define DECIDE_MIN_MS            50
#define DECIDE_MAX_MS           300
#define COOK_MIN_MS             300
#define COOK_MAX_MS             800
#define EAT_MIN_MS             1000
#define EAT_MAX_MS             3000

/* Menu items (no spaces — written verbatim to the log) */
static const char * const MENU[] = {
    "BURGER", "PASTA", "SALAD", "STEAK", "SOUP", "PIZZA", "TACOS", "SUSHI"
};
#define MENU_SIZE ((int)(sizeof MENU / sizeof MENU[0]))

/* ── Customer state machine ───────────────────────────────────────────────────*/

typedef enum {
    STATE_ARRIVING,
    STATE_QUEUED,
    STATE_SEATED,
    STATE_ORDERING,
    STATE_WAITING_FOR_MEAL,
    STATE_EATING,
    STATE_LEAVING,
} CustomerState;

/* String names must match exactly — the grader parses these tokens. */
static const char * const STATE_NAMES[] = {
    "ARRIVING",
    "QUEUED",
    "SEATED",
    "ORDERING",
    "WAITING_FOR_MEAL",
    "EATING",
    "LEAVING",
};

/* ── Per-customer struct ──────────────────────────────────────────────────────*/

typedef struct {
    int            id;
    CustomerState  state;
    int            table_id;    /* -1 until seated               */
    int            waiter_id;   /* -1 until seated               */
    int            meal_ready;  /* 1 when chef has cooked        */

    pthread_mutex_t meal_mutex; /* guards state + meal_ready     */
    pthread_cond_t  meal_cond;  /* two sequential handshakes:
                                 *   1. QUEUED → SEATED (waiter signals)
                                 *   2. WAITING_FOR_MEAL → EATING (chef signals) */
} Customer;

/* ── Per-waiter struct ────────────────────────────────────────────────────────*/

typedef struct {
    int  id;
    int  current_customer_count;

    pthread_mutex_t load_mutex;
    pthread_cond_t  load_cond;
} Waiter;

/* ── Order (waiter → chef) ───────────────────────────────────────────────────*/

typedef struct {
    int         customer_id;   /* -1 = shutdown sentinel */
    int         waiter_id;
    const char *item;
} Order;

/* ── Bounded queue (generic circular buffer) ─────────────────────────────────*/

typedef struct {
    void           **buf;
    int              head, tail, count, capacity;
    pthread_mutex_t  mutex;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
} BoundedQueue;

/* ── Waiting queue (customer IDs) ────────────────────────────────────────────*/

typedef struct {
    int             *buf;
    int              head, tail, count, capacity;
    pthread_mutex_t  mutex;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
} WaitingQueue;

/* ── Order queue ─────────────────────────────────────────────────────────────*/

typedef struct {
    Order           *buf;
    int              head, tail, count, capacity;
    pthread_mutex_t  mutex;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
} OrderQueue;

#endif /* RESTAURANT_H */
