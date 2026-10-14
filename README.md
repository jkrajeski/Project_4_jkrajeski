# Project 4: Restaurant Management Simulation
### *Concurrent Programming with POSIX Threads*

# 1.  Overview
In this project you will implement a multithreaded simulation of a restaurant.  The simulation models three kinds of concurrent actors — customers, waiters, and chefs — whose interactions require you to apply every major POSIX-thread synchronization primitive covered in lecture: mutexes, condition variables, counting semaphores, and bounded producer-consumer queues.
You are given the header file restaurant.h, the thread-safe logging library restaurant_log.h / restaurant_log.c, and the log-format specification.  Your task is to write restaurant.c — the complete implementation of all thread functions, queue operations, and main().
# 2.  Learning Objectives
●* Create and join POSIX threads with pthread_create / pthread_join.  
●* Implement bounded producer-consumer queues using mutex + condition-variable pairs.  
●* Use a counting semaphore to manage a fixed pool of shared resources (tables).  
●* Apply per-thread load limiting with a second condition variable.  
●* Signal a specific waiting thread using per-object condition variables.  
●* Reason about thread termination and avoid deadlock or busy-waiting.  
●* Produce a correctly ordered, timestamped simulation log.  

⠀3.  Architecture
The simulation runs three classes of threads:
### 3.1  Thread Roles
| **Thread type** | **Responsibility** |
|---|---|
| customer_thread | Drives one customer through the state machine from ARRIVING to LEAVING. |
| waiter_thread | Dequeues waiting customers, seats them at a table, takes their order, and submits it to the kitchen. |
| chef_thread | Dequeues orders, sleeps to simulate cooking, then signals the customer directly when the meal is ready. |

### 3.2  Shared Queues
Two bounded FIFO queues connect the thread roles:
| **Queue** | **Flow** |
|---|---|
| WaitingQueue | customer_thread  →  waiter_thread Customer enqueues its own ID after arriving; a waiter dequeues it and seats the customer. |
| OrderQueue | waiter_thread  →  chef_thread Waiter enqueues an Order after the customer has chosen; a chef dequeues and cooks it. |

Both queues are bounded.  If a queue is full the producer must block (not spin) until space is available.  If a queue is empty the consumer must block until an item arrives.
### 3.3  Table Semaphore
The restaurant has a fixed number of tables, controlled by a counting semaphore tables_sem initialised to n_tables.  A waiter calls sem_wait(&tables_sem) before seating a customer and sem_post(&tables_sem) (via release_customer_slot()) when the customer leaves.
### 3.4  Per-Waiter Load Limit
Each waiter may serve at most MAX_CUSTOMERS_PER_WAITER customers simultaneously.  The waiter thread checks current_customer_count under load_mutex before dequeuing a new customer, and blocks on load_cond when the limit is reached.  release_customer_slot() decrements the counter and signals load_cond so the waiter can resume.
# 4.  Customer State Machine
Each customer passes through the following states in strict order:
|   ARRIVING       │  (random arrival delay)       
▼   
QUEUED  ─── customer enqueues self, blocks on meal_cond waiting to be seated       │       │  waiter dequeues, claims table (sem_wait), assigns table_id/waiter_id,       │  sets state = SEATED, signals meal_cond       
▼   
SEATED       │  (random order-decision delay)       
▼   
ORDERING ── customer sets state = ORDERING under meal_mutex, signals meal_cond       │        waiter wakes, enqueues Order on order_queue       
▼   
WAITING_FOR_MEAL ── blocks on meal_cond until meal_ready == 1       │       │  chef dequeues order, cooks (random delay), sets meal_ready = 1,       │  signals meal_cond directly to the customer       
▼   
EATING  ── (random eating delay)       │       
▼   
LEAVING ── logs LEAVING, then calls release_customer_slot() |. 

**Key design point**  The meal_mutex / meal_cond pair is used for two different handshakes: (1) the QUEUED → SEATED handshake between waiter and customer, and (2) the WAITING_FOR_MEAL → EATING handshake between chef and customer.  This is intentional — only one handshake is active at a time for a given customer, so the same mutex and condition variable can safely serve both purposes. |

# 5.  Synchronization Requirements
### 5.1  Bounded Queues
Each queue has a mutex, a not_empty condition variable, and a not_full condition variable.  Enqueue blocks when QUEUE_FULL; dequeue blocks when QUEUE_EMPTY.  Always use pthread_cond_wait inside a **while** loop that re-checks the predicate after waking.
### 5.2  Waiter Ordering Handshake
After seating a customer the waiter must wait for the customer to reach STATE_ORDERING before it can enqueue the Order.  The correct wait condition is:
 pthread_mutex_lock(&c->meal_mutex); while (c->state == STATE_SEATED)          /* wait only while still SEATED */     pthread_cond_wait(&c->meal_cond, &c->meal_mutex); pthread_mutex_unlock(&c->meal_mutex);  

**Common mistake**  Do **not** write while (c->state != STATE_ORDERING).  By the time the waiter re-acquires the mutex, the customer may have already advanced to STATE_WAITING_FOR_MEAL.  Waiting only while STATE_SEATED handles all later states correctly and avoids a lost-signal deadlock.  

### 5.3  Chef-to-Customer Signal
After cooking, the chef signals the customer **directly** — there is no intermediate queue.  The chef acquires the customer's meal_mutex, sets meal_ready = 1, calls pthread_cond_signal(&c->meal_cond), and releases the mutex.  The customer, blocked in WAITING_FOR_MEAL, wakes and transitions to EATING. 
*{* *In a real restaurant a waiter delivers the meal, but in this simulation the chef signals the customer directly when the meal is ready. This simplification removes a coordination problem that doesn't add new synchronization concepts — your waiter's job ends after placing the order. }*
### 5.4  Termination
Use a global _Atomic int sim_done flag (set by main() after all customer threads join).  Every blocking loop in waiter and chef threads must use pthread_cond_timedwait with a 100 ms deadline so threads can wake and check sim_done without needing sentinel values injected into the queues.
Two dequeue functions handle shutdown differently:  
●* waiting_queue_dequeue returns -1 when sim_done is set and the queue is empty.  The waiter thread checks for this and returns from the thread function.  
●* order_queue_dequeue returns a sentinel Order{customer_id = -1} on the same condition.  The chef thread checks ord.customer_id < 0 and exits.

# 6.  release_customer_slot()
After logging STATE_LEAVING, the customer thread calls release_customer_slot().  This function must perform both of the following steps, in order:  
**1.sem_post(&tables_sem)** — returns the physical table to the pool so another customer can be seated.  
**2.Decrement the waiter's load counter** — acquires load_mutex, decrements current_customer_count, signals load_cond, and releases load_mutex.  This allows the waiter to accept the next customer from the waiting queue.

**Why both releases matter**  Omitting the semaphore post causes **table starvation** — new customers can never be seated even when customers are leaving.  Omitting the load-counter decrement causes **waiter stall** — the waiter believes it is still at capacity and refuses to pick up new customers.  Both resources must be released every time a customer departs.  

**Ordering matters**  Call release_customer_slot() **after** logging LEAVING, not before.  If the table is freed first, the log parser may record a new customer seated at that table before the departing customer's LEAVING event appears, which is an invariant violation.  

# 7.  Logging
Use the structured helpers in restaurant_log.h — do not write to stdout or call printf for simulation events.  The grader parses simulation.log and checks the invariants described in restaurant_log.h.
| **Event** | **Logging call** |
|---|---|
| Customer state change | log_customer_state(log, cid, "STATE", tid, msg) tid = -1 before the customer is seated |
| Waiter seats customer | log_waiter_load(log, wid, cid, new_load, msg) |
| Order placed on queue | log_order_queued(log, wid, cid, item_str, qlen) |
| Chef completes meal | log_meal_ready(log, chid, cid, item_str) |
| Waiter load released | log_waiter_load(log, wid, cid, new_load, msg) called from inside release_customer_slot() |
| Simulation end | log_sim_end(log, total_served) |

The log_waiter_load call inside release_customer_slot() is what allows the grader to verify that every waiter's load returns to zero by simulation end.  Omitting it will cause a grader failure even if the underlying counter is correct.
# 8.  Build and Run
**Compile with ThreadSanitizer** gcc -Wall -Wextra -g -fsanitize=thread \     -o restaurant restaurant.c restaurant_log.c -lpthread  # Run with defaults  (20 customers, 5 tables, 3 waiters, 2 chefs) ./restaurant  # Override parameters ./restaurant  <customers>  <tables>  <waiters>  <chefs>  # Validate the log python3 log_parser.py simulation.log |
Your submission must compile **without warnings** and produce **zero ThreadSanitizer data-race reports** for all four grader configurations:
| **Configuration** | **Command** |
|---|---|
| Baseline | ./restaurant 20 5 3 2 |
| Minimal | ./restaurant 5 2 1 1 |
| Table pressure | ./restaurant 15 3 3 2 |
| Waiter load pressure | ./restaurant 20 8 2 2 |

**Array-size constraint**  The global arrays customers[], waiters[], and chefs[] are declared in restaurant.h with fixed compile-time sizes TOTAL_CUSTOMERS=20, NUM_WAITERS=3, and NUM_CHEFS=2.  Command-line arguments must not exceed these values; the table count and queue capacities are heap-allocated and may be any positive integer. |

# 9.  Deliverables and Grading
Submit a single file: restaurant.c.
It must compile against the provided restaurant.h and restaurant_log.c without modification to those files.
| **Component** | **Weight** |
|---|---|
| Compiles without warnings | 10 % |
| Baseline test passes log_parser.py | 30 % |
| Minimal + table-pressure tests pass | 20 % |
| Waiter load-pressure test passes | 30 % |
| Zero ThreadSanitizer warnings (all configurations) | 10 % |

# 10.  Hints and Pitfalls
●* **Always re-check the predicate after pthread_cond_wait returns.**  Spurious wakeups are permitted by POSIX; a while loop is mandatory.  
●* **Use _Atomic int for sim_done.**  A plain int read across threads without a lock is a data race that ThreadSanitizer will report.  
●* **Capture the load snapshot under the mutex.**  Read current_customer_count into a local variable before releasing load_mutex, then pass the local to log_waiter_load.  Reading it after unlock races with release_customer_slot.  
●* **Release resources after logging LEAVING.**  If you call release_customer_slot() before the LEAVING log line, the grader may see a new customer seated at the same table before the previous customer officially departed.  
●* **Seed rand() per thread.**  Seed with time(NULL) XOR'd with the thread ID so threads produce different timing sequences.  
●* **Use nanosleep for millisecond delays.**  The timing constants in restaurant.h are in milliseconds.  Pass ms * 1000000L to tv_nsec — not to sleep(), which takes whole seconds.    
●* **Heap-allocate thread arrays in main().**  Stack arrays declared as pthread_t threads[NUM_CHEFS] overflow if the runtime argument exceeds the compile-time constant.  Use malloc to allocate based on the actual runtime count.  

⠀
# 11.  Academic Integrity
This is an individual assignment.  You may discuss concurrency concepts with classmates but all code you submit must be your own.  Do not post your solution publicly before grades are released.  Violations will be handled per the university academic honesty policy.
