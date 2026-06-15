#!/usr/bin/env python3
"""
cow_demo.py — Copy-on-Write in two forms.

PART 1: Real kernel COW via os.fork()
  Same hardware mechanism as cow_demo.c. Python's os.fork() calls the
  same Linux fork() syscall. The kernel marks pages read-only, detects
  the write, copies the physical page — all invisible to Python.
  Run on Linux/macOS (not Windows).

PART 2: Software COW — the pattern in pure Python
  Share a backing store until a write forces a private copy.
  This is how Redis BGSAVE, persistent data structures, and
  copy-on-write filesystems implement the same idea at a higher level.
"""

import os
import sys


# ── Part 1: Real kernel COW ──────────────────────────────────────────────────

def demo_kernel_cow():
    print("=" * 58)
    print("PART 1: Real kernel COW via os.fork()")
    print("=" * 58)

    data = [42] * 100          # shared list; first element is what we'll watch
    go_r,   go_w   = os.pipe() # parent → child: "go"
    done_r, done_w = os.pipe() # child → parent: "done"

    print(f"\n[parent] data[0] = {data[0]}  address of list: {id(data):#x}")
    print("[parent] forking...\n")

    pid = os.fork()

    if pid == 0:
        # ── child ──────────────────────────────────────────────────────────
        os.close(go_w); os.close(done_r)
        os.read(go_r, 1)   # wait for parent's go signal

        print(f"[child]  data[0] BEFORE write = {data[0]}"
              f"  ← reading parent's page (no copy yet)")

        # This assignment triggers the kernel COW page fault.
        # Python calls __setitem__ which writes to the C array backing
        # the list object — same physical write as *(ptr) = value in C.
        data[0] = 99999

        print(f"[child]  data[0] AFTER  write = {data[0]}"
              f"  ← child now has a private physical copy")

        os.write(done_w, b'x')
        sys.exit(0)

    # ── parent ─────────────────────────────────────────────────────────────
    os.close(go_r); os.close(done_w)
    os.write(go_w, b'g')   # release child
    os.read(done_r, 1)     # wait until child has written

    print(f"\n[parent] data[0] = {data[0]}"
          f"  ← original page untouched; COW kept it intact")
    os.waitpid(pid, 0)


# ── Part 2: Software COW ─────────────────────────────────────────────────────

class COWBuffer:
    """
    A buffer that shares its backing list until a write forces a copy.

    The kernel does the same thing with physical pages:
      - snapshot()       ≈  fork()                (mark pages shared, no copy)
      - _ensure_private() ≈  page fault handler    (copy exactly when needed)
      - write()          ≈  the instruction that triggered the fault

    Real-world uses of this software pattern:
      - Redis BGSAVE: forks the process; parent keeps serving writes
        (COW pages diverge); child streams shared pages to disk cheaply.
      - Persistent/immutable data structures (Clojure vectors, Scala
        collections): structural sharing + COW for O(1) snapshots.
      - ZFS / btrfs: block-level COW — never overwrite a block in place,
        write the new version to a free block, update the pointer.
      - Python strings: small string interning shares one object until
        a new string is constructed (strings are immutable, so the copy
        happens at construction time rather than write time).
    """

    def __init__(self, data: list):
        self._store = data
        self._owned = True   # True  = we hold a private copy, safe to write
                             # False = sharing with another COWBuffer, must copy on write

    def snapshot(self) -> "COWBuffer":
        """
        Return a snapshot that shares our backing store — O(1), no copy.
        Both buffers are now in 'shared' state; either write triggers a copy.
        """
        self._owned = False          # we give up exclusive ownership
        clone = COWBuffer.__new__(COWBuffer)
        clone._store = self._store   # point at the SAME list object
        clone._owned = False
        print(f"  [snapshot ] sharing store id={id(self._store):#x} — no copy made")
        return clone

    def _ensure_private(self):
        """Triggered on write. Copies the store only if it is shared."""
        if not self._owned:
            old_id = id(self._store)
            self._store = list(self._store)   # ← the "page copy"
            self._owned = True
            print(f"  [COW fault] write to shared store — "
                  f"copied {old_id:#x} → {id(self._store):#x}")

    def write(self, index: int, value) -> None:
        self._ensure_private()
        self._store[index] = value

    def read(self, index: int):
        return self._store[index]

    def store_id(self) -> str:
        return f"{id(self._store):#x}"


def demo_software_cow():
    print("\n" + "=" * 58)
    print("PART 2: Software COW pattern in pure Python")
    print("=" * 58)

    original = COWBuffer([1, 2, 3, 4, 5])
    print(f"\noriginal  store id = {original.store_id()}  data = {original._store}")

    # snapshot() shares the store — O(1), no list copy
    snapshot = original.snapshot()
    print(f"snapshot  store id = {snapshot.store_id()}  (same id = shared)")

    print(f"\nsnapshot reads [0] = {snapshot.read(0)}  ← no copy needed for reads")

    # First write to snapshot triggers the copy
    print("\nsnapshot.write(0, 99):")
    snapshot.write(0, 99)

    print(f"\nAfter write:")
    print(f"  snapshot[0] = {snapshot.read(0)}  (its private copy)")
    print(f"  original[0] = {original.read(0)}  ← unchanged, COW protected it")
    print(f"  snapshot store id = {snapshot.store_id()}")
    print(f"  original store id = {original.store_id()}  (still the old store)")

    # Second write — no copy needed, snapshot already owns its store
    print("\nsnapshot.write(1, 88):  (no copy — already private)")
    snapshot.write(1, 88)
    print(f"  snapshot[1] = {snapshot.read(1)}")
    print(f"  original[1] = {original.read(1)}  ← still untouched")


if __name__ == "__main__":
    demo_kernel_cow()
    demo_software_cow()
