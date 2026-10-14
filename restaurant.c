#define _POSIX_C_SOURCE 200809L

#include "restaurant.h"
#include "restaurant_log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    Customer *customer;
    SimLog *log;
    int tables;
} CustomerArgs;

typedef struct {
    Waiter *waiter;
    SimLog *log;
} WaiterArgs;

typedef struct {
    int id;
    SimLog *log;
} ChefArgs;

static SimLog *simulation_log;

static void delay(unsigned int ms)
{
    struct timespec request;
    request.tv_sec = (time_t)(ms / 1000U);
    request.tv_nsec = (long)(ms % 1000U) * 1000000L;

    while (nanosleep(&request, &request) == -1 && errno == EINTR) {
    }
}

static unsigned int small_delay(unsigned int min_ms, unsigned int max_ms)
{
    static unsigned int sequence = 0;
    unsigned int span = max_ms - min_ms + 1U;
    unsigned int value = __atomic_add_fetch(&sequence, 1U, __ATOMIC_RELAXED);
    return min_ms + (value * 37U) % span;
}

static void *customer_thread(void *arg)
{
    CustomerArgs *args = (CustomerArgs *)arg;
    Customer *c = args->customer;
    SimLog *log = args->log;

    c->state = STATE_ARRIVING;
    log_customer_state(log, c->id, STATE_NAMES[STATE_ARRIVING], -1,
                       "customer arrived");
    delay(small_delay(ARRIVE_MIN_MS, ARRIVE_MAX_MS));

    c->state = STATE_QUEUED;
    log_customer_state(log, c->id, STATE_NAMES[STATE_QUEUED], -1,
                       "customer joined waiting queue");
    delay(20);

    //assign a plausible table/waiter without a real queue
    c->table_id = c->id % args->tables;
    c->waiter_id = 0;
    c->state = STATE_SEATED;
    log_customer_state(log, c->id, STATE_NAMES[STATE_SEATED], c->table_id,
                       "customer seated at table");
    delay(small_delay(DECIDE_MIN_MS, DECIDE_MAX_MS));

    c->state = STATE_ORDERING;
    log_customer_state(log, c->id, STATE_NAMES[STATE_ORDERING], c->table_id,
                       "customer placed order");
    delay(20);

    c->state = STATE_WAITING_FOR_MEAL;
    log_customer_state(log, c->id, STATE_NAMES[STATE_WAITING_FOR_MEAL],
                       c->table_id, "customer waiting for meal");
    delay(small_delay(COOK_MIN_MS, COOK_MAX_MS));

    log_meal_ready(log, 0, c->id, MENU[c->id % MENU_SIZE]);
    c->meal_ready = 1;
    c->state = STATE_EATING;
    log_customer_state(log, c->id, STATE_NAMES[STATE_EATING], c->table_id,
                       "customer received meal");
    delay(50);

    c->state = STATE_LEAVING;
    log_customer_state(log, c->id, STATE_NAMES[STATE_LEAVING], c->table_id,
                       "customer leaving");
    return NULL;
}

static void *waiter_thread(void *arg)
{
    WaiterArgs *args = (WaiterArgs *)arg;
    char message[80];
    (void)snprintf(message, sizeof message, "waiter %d thread started",
                   args->waiter->id);
    log_event(args->log, "WAITER", "%s", message);
    delay(25);
    log_event(args->log, "WAITER", "wid=%d waiter thread finished",
              args->waiter->id);
    return NULL;
}

static void *chef_thread(void *arg)
{
    ChefArgs *args = (ChefArgs *)arg;
    log_event(args->log, "CHEF", "chid=%d chef thread started",
              args->id);
    delay(25);
    log_event(args->log, "CHEF", "chid=%d chef thread finished", args->id);
    return NULL;
}

static int parse_positive(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < 1 || parsed > INT_MAX) {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

int main(int argc, char **argv)
{
    int n_customers = 20, n_tables = 5, n_waiters = 3, n_chefs = 2;
    Customer *customers = NULL;
    Waiter *waiters = NULL;
    ChefArgs *chefs = NULL;
    CustomerArgs *customer_args = NULL;
    WaiterArgs *waiter_args = NULL;
    pthread_t *customer_threads = NULL, *waiter_threads = NULL, *chef_threads = NULL;
    int customers_created = 0, waiters_created = 0, chefs_created = 0;
    int i, status = EXIT_FAILURE;

    if (argc != 1 && argc != 5) {
        fprintf(stderr, "Usage: %s [customers tables waiters chefs]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 5 &&
        (!parse_positive(argv[1], &n_customers) ||
         !parse_positive(argv[2], &n_tables) ||
         !parse_positive(argv[3], &n_waiters) ||
         !parse_positive(argv[4], &n_chefs))) {
        fprintf(stderr, "All arguments must be positive integers.\n");
        return EXIT_FAILURE;
    }
    if (n_customers > TOTAL_CUSTOMERS || n_waiters > NUM_WAITERS ||
        n_chefs > NUM_CHEFS) {
        fprintf(stderr, "Caps: customers <= %d, waiters <= %d, chefs <= %d; tables must be positive.\n",
                TOTAL_CUSTOMERS, NUM_WAITERS, NUM_CHEFS);
        return EXIT_FAILURE;
    }

    customers = calloc((size_t)n_customers, sizeof *customers);
    waiters = calloc((size_t)n_waiters, sizeof *waiters);
    chefs = calloc((size_t)n_chefs, sizeof *chefs);
    customer_args = calloc((size_t)n_customers, sizeof *customer_args);
    waiter_args = calloc((size_t)n_waiters, sizeof *waiter_args);
    customer_threads = calloc((size_t)n_customers, sizeof *customer_threads);
    waiter_threads = calloc((size_t)n_waiters, sizeof *waiter_threads);
    chef_threads = calloc((size_t)n_chefs, sizeof *chef_threads);
    if (!customers || !waiters || !chefs || !customer_args || !waiter_args ||
        !customer_threads || !waiter_threads || !chef_threads) {
        fprintf(stderr, "Failed to allocate simulation arrays.\n");
        goto cleanup;
    }

    simulation_log = log_init("simulation.log", "week1", n_customers,
                              n_tables, n_waiters, n_chefs);
    if (!simulation_log) {
        fprintf(stderr, "Could not open simulation.log.\n");
        goto cleanup;
    }

    for (i = 0; i < n_waiters; ++i) {
        waiters[i].id = i;
        waiters[i].current_customer_count = 0;
        waiter_args[i].waiter = &waiters[i];
        waiter_args[i].log = simulation_log;
        if (pthread_create(&waiter_threads[i], NULL, waiter_thread,
                           &waiter_args[i]) != 0) {
            fprintf(stderr, "Failed to create waiter thread %d.\n", i);
            goto join_threads;
        }
        ++waiters_created;
    }

    for (i = 0; i < n_chefs; ++i) {
        chefs[i].id = i;
        chefs[i].log = simulation_log;
        if (pthread_create(&chef_threads[i], NULL, chef_thread, &chefs[i]) != 0) {
            fprintf(stderr, "Failed to create chef thread %d.\n", i);
            goto join_threads;
        }
        ++chefs_created;
    }

    for (i = 0; i < n_customers; ++i) {
        customers[i].id = i;
        customers[i].state = STATE_ARRIVING;
        customers[i].table_id = -1;
        customers[i].waiter_id = -1;
        customers[i].meal_ready = 0;
        customer_args[i].customer = &customers[i];
        customer_args[i].log = simulation_log;
        customer_args[i].tables = n_tables;
        if (pthread_create(&customer_threads[i], NULL, customer_thread,
                           &customer_args[i]) != 0) {
            fprintf(stderr, "Failed to create customer thread %d.\n", i);
            goto join_threads;
        }
        ++customers_created;
    }

    status = EXIT_SUCCESS;

join_threads:
    for (i = 0; i < customers_created; ++i)
        (void)pthread_join(customer_threads[i], NULL);
    for (i = 0; i < waiters_created; ++i)
        (void)pthread_join(waiter_threads[i], NULL);
    for (i = 0; i < chefs_created; ++i)
        (void)pthread_join(chef_threads[i], NULL);

    if (simulation_log) {
        log_sim_end(simulation_log, customers_created);
        log_destroy(simulation_log);
        simulation_log = NULL;
    }

cleanup:
    free(customers);
    free(waiters);
    free(chefs);
    free(customer_args);
    free(waiter_args);
    free(customer_threads);
    free(waiter_threads);
    free(chef_threads);
    return status;
}
