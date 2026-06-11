# Linux Fundamentals Cheat Sheet — Senior SRE Interview Prep

## 1. The Magic Numbers (pattern: number → power → where it lives → war story)

| Number | Power | What it is in Linux |
|---|---|---|
| 256 | 2^8 | One byte's worth of values |
| 1024 | 2^10 | 1 KiB; ports below 1024 are privileged (root to bind) |
| 4096 | 2^12 | **Memory page size** (4 KiB); default ext4/xfs block size |
| 32768 | 2^15 | **Historical default `pid_max`** (`/proc/sys/kernel/pid_max`) — why old systems wrapped PIDs at 32768; still the kernel's compiled-in default |
| 65536 | 2^16 | **TCP/UDP port space** (ports 0–65535; 16-bit header field). Ephemeral range: `/proc/sys/net/ipv4/ip_local_port_range`, default **32768–60999** (note 2^15 again!) ≈ 28k ports = max concurrent outbound conns per src-IP/dest pair. Exhaustion: "Cannot assign requested address" + TIME_WAIT pileup in `ss -s`; fixes: widen range, `tcp_tw_reuse`, more source IPs, or really — connection pooling |
| 4194304 | 2^22 | **Modern `pid_max`** — systemd (v243+, ~2019) raises it to this at boot; it's the kernel's `PID_MAX_LIMIT` on 64-bit. Verify: `cat /proc/sys/kernel/pid_max` |
| ~4.3B | 2^32 | 32-bit address space / IPv4 address space |

**Say it like:** "2^15 — the classic default pid_max, why old boxes wrapped PIDs at 32768 — though modern systemd raises it to 4194304, which is 2^22, the kernel's hard ceiling. Checked it on my own box this week."

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

---
---

# Part 2 — Terraform / AWS / IAM

## 10. Terraform

**State is THE topic.** The state file maps your config to real-world resource IDs — it's Terraform's memory of what it manages. Senior answers:
- **Remote backend**: S3 bucket (versioned + encrypted) with **DynamoDB state locking** to prevent concurrent applies. State contains secrets in plaintext → encrypt at rest, lock down access like a credential store.
- **`plan` vs `apply`**: plan refreshes state against reality, computes the diff (create/update/destroy), and shows it; apply executes it. Always review plans; in CI, plan output is the review artifact.
- **Drift** (someone clicked in the console): `terraform plan` exposes it as a diff. Choices: let TF revert it, update code to match reality, or `ignore_changes` if it's legitimately managed elsewhere.
- **Adopting existing resources**: `terraform import` (or **`import` blocks**, TF 1.5+) — write the resource block, import the real ID, plan until clean.
- **State surgery**: NEVER hand-edit the JSON. Use `terraform state mv / rm / list`, and **`moved` blocks** for refactors so renames don't destroy/recreate.
- **Horror-story answer** ("state is corrupted/lost"): restore prior version from the S3 versioned bucket; worst case, rebuild via imports. This is why backend versioning is non-negotiable.
- **`count` vs `for_each`**: count is positional — removing item 2 of 5 *shifts and recreates* everything after it. for_each keys by name → stable addresses. Senior default: for_each.
- **Environments**: prefer separate state per env (separate dirs/backends or workspaces) — blast-radius isolation. Know the workspaces-vs-directories tradeoff debate exists.
- **Safety rails**: `lifecycle { prevent_destroy }` on databases, `sensitive = true` on outputs, pinned provider versions, modules for DRY with semantic versioning.

**One-liner:** "Terraform is a state-reconciliation engine — most real-world pain is state management, which is why my backend is versioned S3 with DynamoDB locking and nobody applies from a laptop."

## 11. IAM

- **Users vs roles**: users have long-lived credentials (avoid); **roles are assumed** — STS issues temporary credentials. Modern answer: humans use SSO/Identity Center, machines use roles, access keys are a smell.
- **Every role has TWO policy types**: the **trust policy** (WHO may assume it — principals) and **permission policies** (WHAT it can do). Interviewers love asking why cross-account access fails: usually the trust policy.
- **Policy evaluation order** (recite cold): default deny → **explicit DENY always wins** → otherwise need an explicit Allow that survives SCPs (org guardrails), permission boundaries, and session policies. Resource-based policies (S3 bucket policy, KMS key policy) evaluate alongside identity policies.
- **EC2**: roles attach via **instance profile**; apps get creds from metadata — which is why **IMDSv2** matters (session tokens + hop limit kill the classic SSRF credential-theft path).
- **ECS — the big one**: **task execution role** = what the ECS *agent* uses (pull image from ECR, write CloudWatch logs, fetch secrets). **Task role** = what *your application* uses (S3, SQS, etc.). Mixing these up is a known senior-filter question.
- **Debugging AccessDenied**: identify the principal (CloudTrail shows the denied call + which policy type denied), then walk the chain: identity policy → resource policy → SCP → permission boundary → condition keys (aws:SourceIp, aws:PrincipalOrgID...). Tools: CloudTrail, IAM Policy Simulator, Access Analyzer.
- **Least privilege in practice**: start from Access Analyzer / CloudTrail-generated policies, scope resources with ARNs not `*`, use conditions.

## 12. EC2

- **SG vs NACL**: Security groups are **stateful** (return traffic auto-allowed), allow-rules only, instance-level. NACLs are **stateless** (need both directions), allow+deny, subnet-level. The stateful/stateless word pair is the answer.
- **ASG**: launch template + min/max/desired; health checks (EC2 vs **ELB** — use ELB type so app-dead-but-instance-alive gets replaced); scaling policies (target tracking is the modern default).
- **Status checks**: *system* check fails = AWS's host problem (stop/start to migrate hardware); *instance* check fails = your OS problem (kernel panic, full disk, bad network config).
- **Storage**: EBS persists, instance store dies with the host. **gp2 burst credits** vs gp3 (baseline 3000 IOPS, independently provisionable — the "just convert to gp3" answer). io1/io2 for guaranteed IOPS.
- **T-series CPU credits**: t3 instances baseline + burst; exhausted credits = mystery throttling. Check CPUCreditBalance; "unlimited" mode trades it for cost.
- **Purchasing**: on-demand / spot (interruptible, 2-min warning — fine for stateless ECS capacity) / savings plans & RIs for baseline.
- **IMDSv2**: require it everywhere (metadata-options in the launch template).

## 13. ECS

- **Hierarchy**: cluster → service (keeps N tasks running, wires to ALB) → task (running instance of a **task definition**: image, cpu/mem, ports, roles, logs).
- **Fargate vs EC2 launch type**: Fargate = serverless tasks, no hosts to patch, pay per task, fewer knobs (no daemonsets, size limits). EC2 = you run the container instances, more control/cheaper at scale, you own patching and capacity (capacity providers manage ASG scaling).
- **Deployments**: rolling by default (minimumHealthyPercent / maximumPercent); enable the **deployment circuit breaker** to auto-rollback failed deploys; blue/green via CodeDeploy.
- **Shutdown path** (ties to Linux Part 1): ECS sends **SIGTERM**, waits `stopTimeout` (default 30s), then **SIGKILL** → exit 137. No graceful handler = dropped requests every deploy.
- **Debugging a task that won't run**: stuck PENDING → no capacity, subnet/ENI exhaustion, or can't pull image (execution role/ECR/NAT path). Crash-looping → check **stoppedReason** on the stopped task + CloudWatch logs; 137 = OOM or stopTimeout kill; **ECS Exec** to shell into a live task.
- **Networking**: awsvpc mode = each task gets its own ENI/SG → ALB targets are task IPs; health check failures usually = SG between ALB and task, wrong port, or app slow to start (tune healthCheckGracePeriod).

## 14. RDS

- **THE distinction** (guaranteed question): **Multi-AZ = availability** — synchronous standby in another AZ, automatic failover (~60–120s, DNS CNAME flips, no reads served). **Read replicas = scale** — asynchronous copies you can read from (watch ReplicaLag); cross-region capable; promotable for DR.
- **Backups**: automated daily snapshot + transaction logs → **point-in-time recovery** within the retention window; manual snapshots persist until deleted. Restores create a NEW instance (practice the DNS/endpoint swap story).
- **Parameter groups**: static params need a reboot, dynamic apply live; never modify the default group — clone it.
- **Connections**: max_connections scales with instance memory; connection storms from Lambda/ECS scale-outs → **RDS Proxy** (or PgBouncer) for pooling. Queued connections present as latency spikes with no errors (Round 2, Q12).
- **Storage perf**: gp2 **BurstBalance** hitting zero = the classic "DB suddenly slow, CPU idle" page (DiskQueueDepth climbs, read latency jumps). Fix: gp3/provisioned IOPS (online change). Long-term: alert on BurstBalance.
- **Monitoring set**: CPUUtilization, DatabaseConnections, Read/WriteLatency, DiskQueueDepth, FreeableMemory, FreeStorageSpace, ReplicaLag + **Performance Insights** for top waits/queries.
- **Aurora** (one breath): shared distributed storage across 3 AZs, replicas share storage so lag is tiny, failover faster, reader/writer endpoints.

## 15. The SRE Layer (rest of the colleague's list, condensed)

- **SLI** = measurement (p99 latency, success rate). **SLO** = target (99.9% over 30d). **SLA** = contract with penalties. **Error budget** = 1 − SLO: budget left → ship features; budget burned → freeze and fix reliability. This framing turns dev-vs-ops fights into math.
- **Four golden signals**: latency, traffic, errors, **saturation**. Alert on *symptoms* (user-facing SLO burn), not causes (CPU%).
- **Toil**: manual, repetitive, automatable, scales with growth — the job is eliminating it.
- **Incident flow**: ack → severity → IC + comms roles → **mitigate first** (rollback/failover/scale), root-cause later → blameless postmortem with owned action items. Metrics: MTTD/MTTR.
- **PagerDuty**: escalation policies, schedules/overrides, and a story about killing noisy alerts (alert fatigue) scores points.
- **New Relic**: APM transactions/distributed traces, NRQL shape — `SELECT percentile(duration, 99) FROM Transaction WHERE appName='X' FACET host TIMESERIES` — alert conditions tied to SLOs.
