# Linux Fundamentals Cheat Sheet — Senior SRE Interview Prep

## 1. The Magic Numbers (pattern: number → power → where it lives → war story)

| Number | Power | What it is in Linux |
|---|---|---|
| 256 | 2^8 | One byte's worth of values |
| 1024 | 2^10 | 1 KiB; ports below 1024 are privileged (root to bind) |
| 4096 | 2^12 | **Memory page size** (4 KiB); default ext4/xfs block size |
| 32768 | 2^15 | **Default `pid_max`** (`/proc/sys/kernel/pid_max`) — why PIDs wrap at 32768 |
| 65536 | 2^16 | **TCP/UDP port space** (ports 0–65535; 16-bit header field). Ephemeral range: `/proc/sys/net/ipv4/ip_local_port_range` |
| ~4.3B | 2^32 | 32-bit address space / IPv4 address space |

**Say it like:** "2^15 — that's the default pid_max; I've raised it on hosts running thousands of containers."

---

## 2. The Memory Story (one connected narrative)

- RAM is managed in **4096-byte pages**. Virtual pages aren't backed by physical RAM until first touch (**demand paging**).
- **RSS** = pages actually resident in RAM (working set). **VSZ** = total virtual address space mapped. Huge VSZ + small RSS is normal (JVMs, Go).
- Spare RAM is used as **page cache** — caching disk reads/writes. It is *evicted instantly* when apps need memory.
- Therefore in `free -m`, "used" is mostly cache and **the number that matters is `available`** (free + reclaimable cache). 500MB "free" on a 32GB box is healthy. ("Linux ate my RAM" trap.)
- **`vm.swappiness`** makes the kernel proactively swap cold anonymous pages to preserve page cache — so a box with "free" RAM can still be swapping. Apps never "choose" swap.
- When the kernel needs memory and can't reclaim any, the **OOM killer** fires. Victim chosen by **oom_score** (memory footprint, tuned by `oom_score_adj`, −1000 to +1000). Protect critical procs: `oom_score_adj = -1000` or systemd `OOMScoreAdjust=`.
- **Containers:** a cgroup memory limit is its own little world. Hit it → OOM-killed inside the cgroup **even if the host has free RAM** → **exit code 137 (128 + 9 = SIGKILL)**. Classic aggravator: old JVM sizing heap from host RAM. Fix: raise task memory, set `-Xmx` / `-XX:MaxRAMPercentage`.

---

## 3. Process Lifecycle

**The skeleton:** `fork()` (clone) → child `execve()` replaces its image → dynamic linker maps libc → syscalls do the work → `exit()` → parent `wait4()` reaps, gets SIGCHLD.
For `ls`: fork → exec /bin/ls → `openat()` + `getdents64()` on the dir (+ `stat()` per file if `-l`) → `write()` to stdout → exit → reaped. *"You can watch it all with `strace -f`."*

**Process states (`ps` STAT column):**

| State | Meaning |
|---|---|
| R | Running or runnable (on run queue) |
| S | Interruptible sleep (waiting on an event — most procs most of the time) |
| **D** | **Uninterruptible sleep — blocked in a kernel syscall, almost always I/O. Immune to kill -9. Counts toward load average.** |
| **Z** | **Zombie — already exited; just a PID + exit status awaiting parent's `wait()`. Uses no resources. Doesn't count toward load.** |
| T | Stopped (SIGSTOP / ctrl-Z) |
| I | Idle kernel thread |

Suffix flags (not states): `s` = session leader, `l` = multithreaded, `+` = foreground. So `Ssl` = sleeping, session leader, multithreaded.

**The three lookalikes — never fuse these again:**
- **D state:** ALIVE, stuck mid-syscall on I/O. *"Dying to finish."*
- **Zombie (Z):** DEAD, unreaped. *"Dead, waiting to be buried."*
- **Orphan:** alive, parent died → adopted by init/systemd (which reaps it when it exits).

**The symmetry sentence (recite cold):** *A zombie is freed by its PARENT (wait); a D-state process is freed by its I/O (completion or error).* One's fate is upstream, the other's downstream.

**Zombie pileup:** parent isn't calling `wait()`. Each zombie holds a PID → eventually fork() fails. Fix the PARENT (or restart it — init adopts and reaps all). Never "kill" zombies; they're already dead.

**fork() fails with EAGAIN ("Resource temporarily unavailable") — three limits to name:**
1. Per-user `ulimit -u` (RLIMIT_NPROC) — most common
2. Kernel-wide `pid_max`
3. cgroup `pids.max` (systemd `TasksMax=`)
Confirm: `ps -eLf | wc -l`, `ulimit -u`, `pids.current` in the cgroup, `ps aux | grep -c defunct` (zombie check).

---

## 4. Signals

- `kill` sends **SIGTERM (15)** by default — *catchable*: app can drain connections, flush, exit clean.
- **SIGKILL (9)** and **SIGSTOP (19)** can NEVER be caught, ignored, or blocked — kernel-enforced.
- **Caught** = registered handler runs. **Ignored** = SIG_IGN, discarded. **Blocked** = held pending via `sigprocmask`, delivered on unblock.
- SRE tie-in: ECS sends SIGTERM, waits `stopTimeout`, then SIGKILL — no graceful handler = dropped requests every deploy.
- kill -9 on a D-state proc: the signal is **queued, not ignored** — it lands the instant the syscall returns. (Mechanism, not just rule.)

---

## 5. D-State / Load Average Playbook

- **Load average = runnable (R) + uninterruptible (D)** — NOT just CPU. Load 45 with 92% idle CPU = ~45 procs stuck on I/O (classically one hung NFS mount gluing every toucher).
- Always split the diagnosis: `vmstat` / `top` — CPU-bound (high us/sy) vs I/O (high wa, D states)?
- Find culprits: `ps aux | awk '$8 ~ /^D/'` · what they're stuck in: `cat /proc/<pid>/stack` or WCHAN column (`ps -o pid,stat,wchan,cmd`) · `dmesg` ("nfs: server not responding", I/O errors) · `iostat -x` (disk at 100% util?). Caveat that scores points: **lsof itself can hang** on the dead mount.
- A D-state process goes away ONLY by: (1) I/O completes, (2) I/O errors out (`umount -f` / `umount -l`, storage timeout), (3) reboot.
- Hung `ls` = same story: stat()ing entries on a dead NFS mount. `strace ls` shows the blocked syscall. (If only `ls -l` hangs: NSS/LDAP lookups.)

---

## 6. Files, FDs, Inodes

- **Inode** = file metadata + data-block pointers; the filename is just a directory entry pointing at it.
- **Hard link** = another directory entry → same inode. Data freed only when link count = 0 AND no open FDs. Can't cross filesystems or link directories.
- **Symlink** = tiny file containing a path. Delete the target → dangles.
- "Disk full" two ways: **`df` vs `du` mismatch** → deleted-but-open files (`lsof | grep deleted`, restart holder). **Inode exhaustion** → `df -i` (millions of tiny files).
- "Too many open files" (EMFILE — an *open()* error, NOT a fork error): band-aid `ulimit -n`; permanent: systemd `LimitNOFILE=` or `/etc/security/limits.conf`; system ceiling `fs.file-max`. Usual culprit: leaked sockets (CLOSE_WAIT pileup — check `ss` / `lsof -p`).

---

## 7. DNS Resolution Path

- Apps resolve via libc **`getaddrinfo()`** → obeys **`/etc/nsswitch.conf`** (`hosts: files dns` — "files/dns", not old Solaris "bind/local") → `/etc/hosts` → resolver (often **systemd-resolved stub at 127.0.0.53**) → `/etc/resolv.conf`.
- **`dig` bypasses all of that** and queries DNS directly. So "dig works, app doesn't" = the libc/NSS path is broken, not DNS.
- Also check: JVM caches DNS forever by default; containers have their own resolv.conf.

---

## 8. Kernel, One Breath

"The kernel manages hardware on behalf of user space — process scheduling, virtual memory, filesystems, networking, drivers. User space crosses into kernel space via syscalls, which is exactly what strace lets you watch."

---

## 9. One-Liners Worth Saying Out Loud

- "Linux load average counts runnable **plus** uninterruptible — high load with an idle CPU means look at storage, not CPU."
- "Exit 137 is 128+9 — the cgroup OOM-killed it; host memory is irrelevant inside a cgroup."
- "free's 'available' is the real number — 'used' is mostly page cache the kernel will hand back instantly."
- "A zombie is freed by its parent; a D-state process is freed by its I/O."
- "kill -9 on D state isn't ignored — it's queued until the syscall returns."
- "dig bypasses nsswitch; the app doesn't."
- "fork fails on PIDs and nproc; open fails on file descriptors — different limits, different errors."
