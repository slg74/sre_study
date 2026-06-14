/*
 * cow_demo.c — demonstrates Copy-on-Write (COW) via fork().
 *
 * Compile: gcc -O0 -o cow_demo cow_demo.c && ./cow_demo
 *
 * How COW works:
 *
 *   After fork(), the kernel does NOT copy the parent's memory. Instead it
 *   marks every writable page in both page tables as read-only and bumps a
 *   reference count. Parent and child share the same physical pages.
 *
 *   On the first write to any shared page, the CPU raises a page fault.
 *   The kernel's fault handler:
 *     1. Allocates a fresh physical page
 *     2. Copies the shared page's content into it
 *     3. Updates the faulting process's page table entry to the new page
 *     4. Marks the new page read-write (the faulting process's copy)
 *     5. Returns to user space — the write retries and succeeds
 *
 *   Result: same virtual address in both processes, different physical pages,
 *   neither process sees the other's write. The fault is invisible.
 *
 *   This is why fork() is O(1) regardless of process size. A 4 GB process
 *   forks in microseconds. Only touched pages are ever physically copied.
 *
 *   MAP_PRIVATE gives the same guarantee for mmap'd regions.
 *
 * Sequence in this program:
 *
 *   parent writes 42  →  forks
 *                              child reads: sees 42  (shared page, no copy yet)
 *                              child writes 99       (COW fault, new page)
 *                              child reads: sees 99  (its private copy)
 *   parent reads: sees 42  (original page untouched)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

int main(void) {
    /* Two pipes for synchronisation so output is deterministic. */
    int go[2], done[2];
    pipe(go);
    pipe(done);

    /*
     * MAP_PRIVATE | MAP_ANONYMOUS: a zero-initialised page with COW semantics.
     * Writes in one process are never visible to others sharing the mapping.
     */
    int *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 1; }

    *page = 42;
    printf("[parent] wrote 42  →  vaddr %p\n", (void *)page);
    printf("[parent] forking...\n\n");

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* ── child ── */
        char c;
        read(go[0], &c, 1);   /* wait for parent's go signal */

        printf("[child]  vaddr %p  value BEFORE write = %d"
               "  ← shared physical page, no copy yet\n",
               (void *)page, *page);

        /*
         * This write triggers the COW fault.
         * The kernel copies the physical page here — invisible to us.
         */
        *page = 99;

        printf("[child]  vaddr %p  value AFTER  write = %d"
               "  ← child now owns a private physical copy\n",
               (void *)page, *page);

        write(done[1], "x", 1);  /* signal parent */
        exit(0);
    }

    /* ── parent ── */
    write(go[1], "g", 1);    /* release child */

    char c;
    read(done[0], &c, 1);    /* wait until child has written */

    printf("\n[parent] vaddr %p  value = %d"
           "  ← original page untouched; COW kept it intact\n",
           (void *)page, *page);

    wait(NULL);
    munmap(page, 4096);
    return 0;
}
