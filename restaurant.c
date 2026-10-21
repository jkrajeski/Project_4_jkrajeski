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

static Customer *all_customers;
static Waiter *all_waiters;

static int customer_count;
static int table_count;

static WaitingQueue waiting_queue;
static OrderQueue order_queue;

static sem_t table_semaphore;
static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static int *table_in_use;

//flags for shutdown and errors
static _Atomic int sim_done = 0;
static _Atomic int abort_simulation = 0;

static struct timespec deadline_100ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_nsec += 100000000L;

    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    return ts;
}

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

static int stopping(void)
{
    return atomic_load_explicit(
        &sim_done, memory_order_relaxed) != 0;
}

static int aborted(void)
{
    return atomic_load_explicit(
        &abort_simulation, memory_order_relaxed) != 0;
}

//WAITING QUEUE
//he waiting queue stores customer IDs.
//Mutex Protects the queue's head, tail, and count.
//not_empty Signals waiters when a customer arrives.
//Signals customers when queue space becomes available.
static int waiting_queue_init(WaitingQueue *q, int capacity)
{
    q->buf = calloc((size_t)capacity, sizeof *q->buf);

    if (q->buf == NULL)
        return 0;

    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->capacity = capacity;

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        free(q->buf);
        q->buf = NULL;
        return 0;
    }

    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        free(q->buf);
        q->buf = NULL;
        return 0;
    }

    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mutex);
        free(q->buf);
        q->buf = NULL;
        return 0;
    }

    return 1;
}

//Add a customer ID to the waiting queue.
//If the queue is full, the customer waits until space is available.
static int waiting_queue_push(WaitingQueue *q, int customer_id)
{
    int ok = 0;

    pthread_mutex_lock(&q->mutex);

    while (q->count == q->capacity &&
           !stopping() && !aborted()) {

        struct timespec deadline = deadline_100ms();

        (void)pthread_cond_timedwait(
            &q->not_full, &q->mutex, &deadline);
    }

    if (!stopping() && !aborted()) {
        q->buf[q->tail] = customer_id;

        q->tail = (q->tail + 1) % q->capacity;
        q->count++;

        /* Wake a waiter that may be waiting for a customer. */
        pthread_cond_signal(&q->not_empty);

        ok = 1;
    }

    pthread_mutex_unlock(&q->mutex);

    return ok;
}

//Remove the oldest customer ID from the waiting queue.
//A waiter sleeps when the queue is empty instead of repeatedly checking it in a busy loop.
static int waiting_queue_pop(WaitingQueue *q, int *customer_id)
{
    int ok = 0;

    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !stopping() && !aborted()) {
        struct timespec deadline = deadline_100ms();

        (void)pthread_cond_timedwait(
            &q->not_empty, &q->mutex, &deadline);
    }

    if (q->count > 0 && !aborted()) {
        *customer_id = q->buf[q->head];

        q->head = (q->head + 1) % q->capacity;
        q->count--;

        //Wake a customer waiting for queue space
        pthread_cond_signal(&q->not_full);

        ok = 1;
    }

    pthread_mutex_unlock(&q->mutex);

    return ok;
}

//Waiters add orders.
//Chefs remove orders.
//The queue is FIFO and protected by a mutex and condition variables.
static int order_queue_init(OrderQueue *q, int capacity)
{
    q->buf = calloc((size_t)capacity, sizeof *q->buf);

    if (q->buf == NULL)
        return 0;

    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->capacity = capacity;

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        free(q->buf);
        q->buf = NULL;
        return 0;
    }

    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        free(q->buf);
        q->buf = NULL;
        return 0;
    }

    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mutex);
        free(q->buf);
        q->buf = NULL;
        return 0;
    }

    return 1;
}

//Enqueue an order and return the queue length after insertion.
static int order_queue_push(OrderQueue *q,
                            const Order *order,
                            int *new_length)
{
    int ok = 0;

    pthread_mutex_lock(&q->mutex);

    while (q->count == q->capacity &&
           !stopping() && !aborted()) {

        struct timespec deadline = deadline_100ms();

        (void)pthread_cond_timedwait(
            &q->not_full, &q->mutex, &deadline);
    }

    if (!stopping() && !aborted()) {
        q->buf[q->tail] = *order;

        q->tail = (q->tail + 1) % q->capacity;
        q->count++;

        *new_length = q->count;

        /* Wake a chef waiting for an order. */
        pthread_cond_signal(&q->not_empty);

        ok = 1;
    }

    pthread_mutex_unlock(&q->mutex);

    return ok;
}

//Remove the oldest order for a chef to prepare.
static int order_queue_pop(OrderQueue *q, Order *order)
{
    int ok = 0;

    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !stopping() && !aborted()) {
        struct timespec deadline = deadline_100ms();

        (void)pthread_cond_timedwait(
            &q->not_empty, &q->mutex, &deadline);
    }

    if (q->count > 0 && !aborted()) {
        *order = q->buf[q->head];

        q->head = (q->head + 1) % q->capacity;
        q->count--;

        /* Wake a waiter that may be waiting to enqueue an order. */
        pthread_cond_signal(&q->not_full);

        ok = 1;
    }

    pthread_mutex_unlock(&q->mutex);

    return ok;
}

//Reserve one workload slot for a waiter.
//A waiter may serve no more than MAX_CUSTOMERS_PER_WAITER customers simultaneously.
static int acquire_waiter_slot(Waiter *w)
{
    pthread_mutex_lock(&w->load_mutex);

    while (w->current_customer_count >= MAX_CUSTOMERS_PER_WAITER &&
           !stopping() && !aborted()) {

        struct timespec deadline = deadline_100ms();

        (void)pthread_cond_timedwait(
            &w->load_cond, &w->load_mutex, &deadline);
    }

    if (stopping() || aborted()) {
        pthread_mutex_unlock(&w->load_mutex);
        return 0;
    }

    w->current_customer_count++;

    pthread_mutex_unlock(&w->load_mutex);

    return 1;
}

//Undo a waiter slot if seating fails before a customer is seated
static void undo_waiter_slot(Waiter *w)
{
    pthread_mutex_lock(&w->load_mutex);

    if (w->current_customer_count > 0)
        w->current_customer_count--;

    pthread_cond_broadcast(&w->load_cond);

    pthread_mutex_unlock(&w->load_mutex);
}

//Reserve a real table using a semaphore.
//The semaphore limits the number of occupied tables.
//The bitmap identifies the specific available table.
static int acquire_table(int *table_id)
{
    for (;;) {
        struct timespec deadline;

        if (stopping() || aborted())
            return 0;

        deadline = deadline_100ms();

        if (sem_timedwait(&table_semaphore, &deadline) == 0)
            break;

        if (errno != EINTR && errno != ETIMEDOUT)
            return 0;
    }

    pthread_mutex_lock(&table_mutex);

    for (int i = 0; i < table_count; ++i) {
        if (!table_in_use[i]) {
            table_in_use[i] = 1;
            *table_id = i;

            pthread_mutex_unlock(&table_mutex);

            return 1;
        }
    }

    pthread_mutex_unlock(&table_mutex);

    //Defensive recovery if semaphore and bitmap disagree
    sem_post(&table_semaphore);

    return 0;
}

//Release a table so another customer can use it
static void release_table(int table_id)
{
    if (table_id < 0 || table_id >= table_count)
        return;

    pthread_mutex_lock(&table_mutex);

    table_in_use[table_id] = 0;

    pthread_mutex_unlock(&table_mutex);

    sem_post(&table_semaphore);
}

//Called after a customer finishes eating and leaves.
//Releases both the table and the waiter's workload slot.
static void release_customer_slot(Customer *c)
{
    int tid = c->table_id;
    int wid = c->waiter_id;

    release_table(tid);

    if (wid >= 0 && wid < NUM_WAITERS) {
        Waiter *w = &all_waiters[wid];
        int load;

        pthread_mutex_lock(&w->load_mutex);

        if (w->current_customer_count > 0)
            w->current_customer_count--;

        load = w->current_customer_count;

        pthread_cond_broadcast(&w->load_cond);

        pthread_mutex_unlock(&w->load_mutex);

        log_waiter_load(
            simulation_log, wid, c->id, load,
            "waiter released customer");
    }

    pthread_mutex_lock(&c->meal_mutex);

    c->table_id = -1;
    c->waiter_id = -1;

    pthread_mutex_unlock(&c->meal_mutex);
}

//Wake threads blocked on condition variables during shutdown.
static void wake_all_waiters(void)
{
    pthread_mutex_lock(&waiting_queue.mutex);

    pthread_cond_broadcast(&waiting_queue.not_empty);
    pthread_cond_broadcast(&waiting_queue.not_full);

    pthread_mutex_unlock(&waiting_queue.mutex);

    pthread_mutex_lock(&order_queue.mutex);

    pthread_cond_broadcast(&order_queue.not_empty);
    pthread_cond_broadcast(&order_queue.not_full);

    pthread_mutex_unlock(&order_queue.mutex);

    for (int i = 0; i < NUM_WAITERS; ++i) {
        pthread_mutex_lock(&all_waiters[i].load_mutex);

        pthread_cond_broadcast(&all_waiters[i].load_cond);

        pthread_mutex_unlock(&all_waiters[i].load_mutex);
    }

    for (int i = 0; i < customer_count; ++i) {
        pthread_mutex_lock(&all_customers[i].meal_mutex);

        pthread_cond_broadcast(&all_customers[i].meal_cond);

        pthread_mutex_unlock(&all_customers[i].meal_mutex);
    }
}

//the customer waits for the waiter and chef to signal the transitions
static void *customer_thread(void *arg)
{
    CustomerArgs *args = (CustomerArgs *)arg;
    Customer *c = args->customer;
    SimLog *log = args->log;

    int seated = 0;

    //ARRIVING
    pthread_mutex_lock(&c->meal_mutex);

    c->state = STATE_ARRIVING;

    pthread_mutex_unlock(&c->meal_mutex);

    log_customer_state(
        log, c->id, STATE_NAMES[STATE_ARRIVING], -1,
        "customer arrived");

    delay(small_delay(ARRIVE_MIN_MS, ARRIVE_MAX_MS));

    if (aborted())
        return NULL;

    //QUEUED
    pthread_mutex_lock(&c->meal_mutex);

    c->state = STATE_QUEUED;

    pthread_mutex_unlock(&c->meal_mutex);

    log_customer_state(
        log, c->id, STATE_NAMES[STATE_QUEUED], -1,
        "customer joined waiting queue");

    //Enter the real waiting queue
    if (!waiting_queue_push(&waiting_queue, c->id))
        return NULL;

    //Wait for the waiter to set STATE_SEATED and signal meal_cond.
    pthread_mutex_lock(&c->meal_mutex);

    while (c->state == STATE_QUEUED && !aborted()) {
        struct timespec deadline = deadline_100ms();

        (void)pthread_cond_timedwait(
            &c->meal_cond, &c->meal_mutex, &deadline);
    }

    if (c->state != STATE_SEATED) {
        pthread_mutex_unlock(&c->meal_mutex);
        return NULL;
    }

    seated = 1;

    pthread_mutex_unlock(&c->meal_mutex);

    //SEATED
    log_customer_state(
        log, c->id, STATE_NAMES[STATE_SEATED], c->table_id,
        "customer seated at table");

    delay(small_delay(DECIDE_MIN_MS, DECIDE_MAX_MS));

    //ORDERING
    pthread_mutex_lock(&c->meal_mutex);

    c->state = STATE_ORDERING;

    pthread_mutex_unlock(&c->meal_mutex);

    log_customer_state(
        log, c->id, STATE_NAMES[STATE_ORDERING], c->table_id,
        "customer placed order");

    delay(20);

    //WAITING_FOR_MEAL
    pthread_mutex_lock(&c->meal_mutex);

    c->state = STATE_WAITING_FOR_MEAL;

    //Wake the waiter, which is waiting for the customer's order.
    pthread_cond_broadcast(&c->meal_cond);

    pthread_mutex_unlock(&c->meal_mutex);

    log_customer_state(
        log, c->id, STATE_NAMES[STATE_WAITING_FOR_MEAL],
        c->table_id, "customer waiting for meal");

    //Wait until a chef sets meal_ready and signals meal_cond.
    pthread_mutex_lock(&c->meal_mutex);

    while (!c->meal_ready && !aborted()) {
        struct timespec deadline = deadline_100ms();

        (void)pthread_cond_timedwait(
            &c->meal_cond, &c->meal_mutex, &deadline);
    }

    if (!c->meal_ready) {
        pthread_mutex_unlock(&c->meal_mutex);

        if (seated)
            release_customer_slot(c);

        return NULL;
    }

    //EATING
    c->state = STATE_EATING;

    pthread_mutex_unlock(&c->meal_mutex);

    log_customer_state(
        log, c->id, STATE_NAMES[STATE_EATING], c->table_id,
        "customer received meal");

    delay(small_delay(EAT_MIN_MS, EAT_MAX_MS));

    //LEAVING
    pthread_mutex_lock(&c->meal_mutex);

    c->state = STATE_LEAVING;

    pthread_mutex_unlock(&c->meal_mutex);

    log_customer_state(
        log, c->id, STATE_NAMES[STATE_LEAVING], c->table_id,
        "customer leaving");

    //Free table and waiter capacity
    if (seated)
        release_customer_slot(c);

    return NULL;
}

static void *waiter_thread(void *arg)
{
    WaiterArgs *args = (WaiterArgs *)arg;

    Waiter *w = args->waiter;

    int cid;

    log_event(
        args->log, "WAITER",
        "wid=%d waiter thread started", w->id);

    while (waiting_queue_pop(&waiting_queue, &cid)) {
        Customer *c = &all_customers[cid];

        int tid = -1;
        int slot_reserved = 0;
        int table_reserved = 0;
        int seated = 0;

        //Reserve a slot under the waiter's workload limit
        if (!acquire_waiter_slot(w))
            break;

        slot_reserved = 1;

        //Reserve an available table
        if (!acquire_table(&tid)) {
            undo_waiter_slot(w);
            break;
        }

        table_reserved = 1;

        //The waiter changes the state to SEATED and signals the customer.
        pthread_mutex_lock(&c->meal_mutex);

        if (c->state == STATE_QUEUED && !aborted()) {
            c->table_id = tid;
            c->waiter_id = w->id;
            c->state = STATE_SEATED;

            seated = 1;

            pthread_cond_broadcast(&c->meal_cond);
        }

        pthread_mutex_unlock(&c->meal_mutex);

        if (!seated) {
            if (table_reserved)
                release_table(tid);

            if (slot_reserved)
                undo_waiter_slot(w);

            if (aborted())
                break;

            continue;
        }

        //Log the waiter's updated workload
        pthread_mutex_lock(&w->load_mutex);

        int load = w->current_customer_count;

        pthread_mutex_unlock(&w->load_mutex);

        log_waiter_load(
            args->log, w->id, cid, load,
            "waiter seated customer");

        //Wait for the customer to finish ordering
        pthread_mutex_lock(&c->meal_mutex);

        while (c->state != STATE_WAITING_FOR_MEAL && !aborted()) {
            struct timespec deadline = deadline_100ms();

            (void)pthread_cond_timedwait(
                &c->meal_cond, &c->meal_mutex, &deadline);
        }

        pthread_mutex_unlock(&c->meal_mutex);

        if (aborted())
            break;

        //Create the customer's order
        Order order;

        order.customer_id = cid;
        order.waiter_id = w->id;
        order.item = MENU[cid % MENU_SIZE];

        int qlen = 0;

        //Put the order in the kitchen queue instead of treating cooking as a delay inside the customer thread
        if (!order_queue_push(&order_queue, &order, &qlen))
            break;

        log_order_queued(
            args->log, w->id, cid, order.item, qlen);
    }

    log_event(
        args->log, "WAITER",
        "wid=%d waiter thread finished", w->id);

    return NULL;
}

/* A chef now:
 *   1. Removes an order from the kitchen queue.
 *   2. Simulates cooking.
 *   3. Logs the completed meal.
 *   4. Signals the correct customer.
 */

static void *chef_thread(void *arg)
{
    ChefArgs *args = (ChefArgs *)arg;

    Order order;

    log_event(
        args->log, "CHEF",
        "chid=%d chef thread started", args->id);

    //Process orders until shutdown
    while (order_queue_pop(&order_queue, &order)) {
        if (aborted())
            break;

        delay(small_delay(COOK_MIN_MS, COOK_MAX_MS));

        Customer *c = &all_customers[order.customer_id];

        log_meal_ready(
            args->log, args->id,
            order.customer_id, order.item);

        //Set meal_ready while holding the customer's mutex, then wake the customer waiting for the meal
        pthread_mutex_lock(&c->meal_mutex);

        c->meal_ready = 1;

        pthread_cond_broadcast(&c->meal_cond);

        pthread_mutex_unlock(&c->meal_mutex);
    }

    log_event(
        args->log, "CHEF",
        "chid=%d chef thread finished", args->id);

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

//Destroy only the synchronization objects that were successfully initialized
//This frees the dynamically allocated queue buffers and table bitmap
static void destroy_sync_objects(
    int customer_mutexes,
    int waiter_mutexes,
    int waiting_queue_ready,
    int order_queue_ready,
    int semaphore_ready)
{
    if (waiting_queue_ready) {
        pthread_cond_destroy(&waiting_queue.not_empty);
        pthread_cond_destroy(&waiting_queue.not_full);
        pthread_mutex_destroy(&waiting_queue.mutex);

        free(waiting_queue.buf);
        waiting_queue.buf = NULL;
    }

    if (order_queue_ready) {
        pthread_cond_destroy(&order_queue.not_empty);
        pthread_cond_destroy(&order_queue.not_full);
        pthread_mutex_destroy(&order_queue.mutex);

        free(order_queue.buf);
        order_queue.buf = NULL;
    }

    if (customer_mutexes) {
        for (int i = 0; i < customer_count; ++i) {
            pthread_cond_destroy(&all_customers[i].meal_cond);
            pthread_mutex_destroy(&all_customers[i].meal_mutex);
        }
    }

    if (waiter_mutexes) {
        for (int i = 0; i < NUM_WAITERS; ++i) {
            pthread_cond_destroy(&all_waiters[i].load_cond);
            pthread_mutex_destroy(&all_waiters[i].load_mutex);
        }
    }

    if (semaphore_ready)
        sem_destroy(&table_semaphore);

    pthread_mutex_destroy(&table_mutex);

    free(table_in_use);
    table_in_use = NULL;
}

int main(int argc, char **argv)
{
    int n_customers = 20;
    int n_tables = 5;
    int n_waiters = 3;
    int n_chefs = 2;

    Customer *customers = NULL;
    Waiter *waiters = NULL;
    ChefArgs *chefs = NULL;

    CustomerArgs *customer_args = NULL;
    WaiterArgs *waiter_args = NULL;

    pthread_t *customer_threads = NULL;
    pthread_t *waiter_threads = NULL;
    pthread_t *chef_threads = NULL;

    int customers_created = 0;
    int waiters_created = 0;
    int chefs_created = 0;

    int customer_sync_ready = 0;
    int waiter_sync_ready = 0;

    int waiting_queue_ready = 0;
    int order_queue_ready = 0;
    int semaphore_ready = 0;

    int i;
    int status = EXIT_FAILURE;

    //Accept no arguments or all four simulation parameters
    if (argc != 1 && argc != 5) {
        fprintf(
            stderr,
            "Usage: %s [customers tables waiters chefs]\n",
            argv[0]);

        return EXIT_FAILURE;
    }

    if (argc == 5 &&
        (!parse_positive(argv[1], &n_customers) ||
         !parse_positive(argv[2], &n_tables) ||
         !parse_positive(argv[3], &n_waiters) ||
         !parse_positive(argv[4], &n_chefs))) {

        fprintf(
            stderr,
            "All arguments must be positive integers.\n");

        return EXIT_FAILURE;
    }

    //Validate against the limits in restaurant.h
    //The number of tables must also be positive
    if (n_customers > TOTAL_CUSTOMERS ||
        n_waiters > NUM_WAITERS ||
        n_chefs > NUM_CHEFS) {

        fprintf(
            stderr,
            "Caps: customers <= %d, waiters <= %d, "
            "chefs <= %d; tables must be positive.\n",
            TOTAL_CUSTOMERS, NUM_WAITERS, NUM_CHEFS);

        return EXIT_FAILURE;
    }

    //Store global dimensions used by the worker threads
    customer_count = n_customers;
    table_count = n_tables;

    //Allocate all arrays on the heap
    //waiter arrays use NUM_WAITERS = maximum waiter count permitted by restaurant.h
    customers = calloc((size_t)n_customers, sizeof *customers);
    waiters = calloc((size_t)NUM_WAITERS, sizeof *waiters);
    chefs = calloc((size_t)n_chefs, sizeof *chefs);

    customer_args = calloc(
        (size_t)n_customers, sizeof *customer_args);

    waiter_args = calloc(
        (size_t)NUM_WAITERS, sizeof *waiter_args);

    customer_threads = calloc(
        (size_t)n_customers, sizeof *customer_threads);

    waiter_threads = calloc(
        (size_t)NUM_WAITERS, sizeof *waiter_threads);

    chef_threads = calloc(
        (size_t)n_chefs, sizeof *chef_threads);

    //Track which tables are occupied
    table_in_use = calloc(
        (size_t)n_tables, sizeof *table_in_use);

    if (!customers ||
        !waiters ||
        !chefs ||
        !customer_args ||
        !waiter_args ||
        !customer_threads ||
        !waiter_threads ||
        !chef_threads ||
        !table_in_use) {

        fprintf(
            stderr,
            "Failed to allocate simulation arrays.\n");

        goto cleanup_allocations;
    }

    all_customers = customers;
    all_waiters = waiters;

    //Initialize each customer's synchronization
    for (i = 0; i < n_customers; ++i) {
        customers[i].id = i;
        customers[i].state = STATE_ARRIVING;
        customers[i].table_id = -1;
        customers[i].waiter_id = -1;
        customers[i].meal_ready = 0;

        if (pthread_mutex_init(
                &customers[i].meal_mutex, NULL) != 0) {
            goto init_failure;
        }

        if (pthread_cond_init(
                &customers[i].meal_cond, NULL) != 0) {

            pthread_mutex_destroy(&customers[i].meal_mutex);

            goto init_failure;
        }

        customer_sync_ready++;
    }

    //Initialize waiter workload synchronization
    for (i = 0; i < NUM_WAITERS; ++i) {
        waiters[i].id = i;
        waiters[i].current_customer_count = 0;

        if (pthread_mutex_init(
                &waiters[i].load_mutex, NULL) != 0) {
            goto init_failure;
        }

        if (pthread_cond_init(
                &waiters[i].load_cond, NULL) != 0) {

            pthread_mutex_destroy(&waiters[i].load_mutex);

            goto init_failure;
        }

        waiter_sync_ready++;
    }

    //Initialize the two real queues
    if (!waiting_queue_init(&waiting_queue, n_customers))
        goto init_failure;

    waiting_queue_ready = 1;

    if (!order_queue_init(&order_queue, n_customers))
        goto init_failure;

    order_queue_ready = 1;

    //Start with every table available
    if (sem_init(
            &table_semaphore, 0, (unsigned int)n_tables) != 0) {
        goto init_failure;
    }

    semaphore_ready = 1;

    //Initialize the required simulation log
    simulation_log = log_init(
        "simulation.log",
        "week1",
        n_customers,
        n_tables,
        n_waiters,
        n_chefs);

    if (!simulation_log) {
        fprintf(
            stderr,
            "Could not open simulation.log.\n");

        goto init_failure;
    }

    //Start waiter threads
    for (i = 0; i < n_waiters; ++i) {
        waiter_args[i].waiter = &waiters[i];
        waiter_args[i].log = simulation_log;

        if (pthread_create(
                &waiter_threads[i],
                NULL,
                waiter_thread,
                &waiter_args[i]) != 0) {

            fprintf(
                stderr,
                "Failed to create waiter thread %d.\n",
                i);

            goto thread_creation_failure;
        }

        waiters_created++;
    }

    //Start chef threads
    for (i = 0; i < n_chefs; ++i) {
        chefs[i].id = i;
        chefs[i].log = simulation_log;

        if (pthread_create(
                &chef_threads[i],
                NULL,
                chef_thread,
                &chefs[i]) != 0) {

            fprintf(
                stderr,
                "Failed to create chef thread %d.\n",
                i);

            goto thread_creation_failure;
        }

        chefs_created++;
    }

    //Start customer threads
    for (i = 0; i < n_customers; ++i) {
        customer_args[i].customer = &customers[i];
        customer_args[i].log = simulation_log;

        if (pthread_create(
                &customer_threads[i],
                NULL,
                customer_thread,
                &customer_args[i]) != 0) {

            fprintf(
                stderr,
                "Failed to create customer thread %d.\n",
                i);

            goto thread_creation_failure;
        }

        customers_created++;
    }

    //Wait for customer threads to finish
    //In a successful simulation, a customer finishes after leaving the restaurant and releases their table and waiter slot
    for (i = 0; i < customers_created; ++i) {
        (void)pthread_join(customer_threads[i], NULL);
    }

    status = EXIT_SUCCESS;

    goto shutdown_workers;

//If a thread could not be created, signal an abort and wake threads that might be blocked on queues or condition variables
thread_creation_failure:
    atomic_store_explicit(
        &abort_simulation, 1, memory_order_relaxed);

    atomic_store_explicit(
        &sim_done, 1, memory_order_relaxed);

    wake_all_waiters();

    for (i = 0; i < customers_created; ++i) {
        (void)pthread_join(customer_threads[i], NULL);
    }

//Clean shutdown of the worker threads
shutdown_workers:
    atomic_store_explicit(
        &sim_done, 1, memory_order_relaxed);

    wake_all_waiters();

    for (i = 0; i < waiters_created; ++i) {
        (void)pthread_join(waiter_threads[i], NULL);
    }

    for (i = 0; i < chefs_created; ++i) {
        (void)pthread_join(chef_threads[i], NULL);
    }

    //Write the footer and close the log after all threads have ended
    if (simulation_log) {
        log_sim_end(
            simulation_log,
            status == EXIT_SUCCESS ? n_customers : 0);

        log_destroy(simulation_log);
        simulation_log = NULL;
    }

    goto cleanup_sync;

//Handle initialization failures
init_failure:
    fprintf(
        stderr,
        "Failed to initialize simulation synchronization objects.\n");

    if (simulation_log) {
        log_destroy(simulation_log);
        simulation_log = NULL;
    }

//Release initialized synchronization resources
cleanup_sync:
    destroy_sync_objects(
        customer_sync_ready,
        waiter_sync_ready,
        waiting_queue_ready,
        order_queue_ready,
        semaphore_ready);

//Free the arrays
cleanup_allocations:
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