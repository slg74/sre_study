/*
 * futex_mutex.c — mutex built directly on the Linux futex(2) syscall.
 *
 * Linux NOTE: compile and run on Linux (or WSL/container).
 *   gcc -O2 -pthread -o futex_mutex futex_mutex.c && ./futex_mutex
 *
 * How futex works in one sentence:
 *   FUTEX_WAIT atomically checks *addr == val and sleeps if so.
 *   FUTEX_WAKE wakes up to N threads sleeping on addr.
 *   The kernel does nothing else — all the logic lives in userspace.
 *
 * The 3-state mutex (Drepper 2011):
 *
 *   0 = unlocked
 *   1 = locked, no waiters   — winner sets this; unlock needs no syscall
 *   2 = locked, waiters present — any syscall wake is needed on unlock
 *
 * Lock fast path (no contention): CAS 0→1, done, zero syscalls.
 * Lock slow path: set state to 2, call FUTEX_WAIT(2). When woken,
 *   try again. The WAIT checks *addr == 2 atomically — if a racing
 *   unlock already cleared it to 0, WAIT returns EAGAIN and we retry
 *   without sleeping.
 * Unlock: fetch_sub(1). If it was 1→0, no waiters, done. If it was
 *   2→1, there are waiters; reset to 0 and call FUTEX_WAKE(1).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

/* ── futex syscall shim ─────────────────────────────────────────────────── */

static inline long futex_wait(atomic_int *addr, int expected) {
    return syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static inline long futex_wake(atomic_int *addr, int n_wake) {
    return syscall(SYS_futex, addr, FUTEX_WAKE, n_wake, NULL, NULL, 0);
}

/* ── mutex ──────────────────────────────────────────────────────────────── */

typedef struct {
    atomic_int state; /* 0=unlocked, 1=locked, 2=locked+waiters */
} ftx_mutex;

#define FTX_MUTEX_INIT { .state = 0 }

void ftx_lock(ftx_mutex *m) {
    int c = 0;

    /* Fast path: if unlocked (0), grab it (→1) and return immediately. */
    if (atomic_compare_exchange_strong_explicit(
            &m->state, &c, 1,
            memory_order_acquire, memory_order_relaxed))
        return;

    /*
     * Slow path. We lost the race. Mark the mutex contended (→2) so the
     * holder knows to call FUTEX_WAKE on unlock, then sleep.
     *
     * If c was already 2 the exchange still puts 2 back — harmless.
     */
    if (c != 2)
        c = atomic_exchange_explicit(&m->state, 2, memory_order_relaxed);

    while (c != 0) {
        /*
         * Sleep only if state is still 2. If a concurrent unlock raced
         * us and set state back to 0, FUTEX_WAIT sees *addr != 2 and
         * returns EAGAIN immediately — no missed wakeup possible.
         */
        futex_wait(&m->state, 2);
        /* Re-mark contended: another thread may have snuck in. */
        c = atomic_exchange_explicit(&m->state, 2, memory_order_acquire);
    }
}

void ftx_unlock(ftx_mutex *m) {
    /*
     * Subtract 1. Previous value:
     *   1 → 0: no waiters, we're done — zero syscalls on the unlock path.
     *   2 → 1: waiters exist; reset to 0 then wake one.
     */
    if (atomic_fetch_sub_explicit(&m->state, 1, memory_order_release) != 1) {
        atomic_store_explicit(&m->state, 0, memory_order_release);
        futex_wake(&m->state, 1);
    }
}

/* ── demo ───────────────────────────────────────────────────────────────── */

#define NUM_THREADS  8
#define INCREMENTS   1000000

static ftx_mutex counter_lock = FTX_MUTEX_INIT;
static long counter = 0;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < INCREMENTS; i++) {
        ftx_lock(&counter_lock);
        counter++;           /* protected critical section */
        ftx_unlock(&counter_lock);
    }
    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];

    printf("Spawning %d threads, each incrementing %d times...\n",
           NUM_THREADS, INCREMENTS);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, worker, NULL);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    long expected = (long)NUM_THREADS * INCREMENTS;
    printf("counter  = %ld\n", counter);
    printf("expected = %ld\n", expected);
    printf("%s\n", counter == expected ? "PASS — no lost updates" : "FAIL — data race detected");

    return counter != expected;
}
