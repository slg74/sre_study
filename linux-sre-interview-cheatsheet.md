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

## 8. Advanced Linux SRE — Senior-Filter Topics

- **IPC and memory-bound workloads**: `perf stat` IPC < 0.5 on a CPU-bound process = memory-bandwidth bottleneck, not compute. CPU is stalling on cache misses. Confirm with `perf stat -e cache-misses,cache-references`. Fix: data locality, smaller working set, memory-optimized instance.
- **Major vs minor page faults** (`sar -B`): *major* fault (pgmajflt) = page not in RAM → disk I/O required → milliseconds of latency per fault. *Minor* fault (pgminflt) = page in RAM but not mapped yet → cheap. Causes of majors: working set > RAM, swap usage, cold mmap.
- **Dirty writeback tuning**: `vm.dirty_background_ratio` (soft, wakes background flusher, default 10%) vs `vm.dirty_ratio` (hard ceiling, causes synchronous write stall, default 20%). High `Dirty:` in `/proc/meminfo` + pegged kworker = writeback falling behind. Lower both for latency-sensitive DBs; raise for write-heavy batch.
- **TIME_WAIT**: normal and harmless unless port exhaustion occurs. `tcp_fin_timeout` controls duration. `tcp_tw_reuse=1` safely allows reuse for outbound connections. `tcp_tw_recycle` is dangerous (breaks NAT, removed in 4.12) — never set it.
- **`ss` for live sockets**: `ss -tp dst 10.0.1.50:5432` — faster than netstat (reads /proc directly), shows PID+process inline. Add `-n` to skip DNS. `-s` for summary counts per state.
- **futex / strace diagnosis**: a process looping on `futex(FUTEX_WAIT)` is parked waiting on a mutex. If it never returns: high contention or deadlock (thread A waits for lock held by thread B, which waits for thread A). Debug with `gdb -p <pid>` thread bt or `perf lock`.
- **cgroups v2 CPU throttling**: write `<quota> <period>` to `cpu.max`. `200000 100000` = 2 cores. `150000 100000` = 1.5 cores. v1 used separate `cpu.cfs_quota_us` / `cpu.cfs_period_us` files. `cpu.weight` (v2) / `cpu.shares` (v1) are relative — not hard limits.
- **`oom_score_adj`**: range -1000 to +1000. -1000 = OOM killer will never choose this process. +1000 = first target. Kernel score = RSS + swap + adj. Always set sshd to -1000 (you need it to recover). OOMKill visible in `/var/log/kern.log` or `dmesg | grep -i oom`.
- **System-wide tracing without restart**: `bpftrace -e 'tracepoint:syscalls:sys_enter_openat { printf("%s %s\n", comm, str(args->filename)); }'` — zero overhead, no process restart, attaches live. For audit-log use: `auditd` + `-a always,exit -F arch=b64 -S openat`. For path-specific watches: `inotifywait`.
- **Load average decay**: load average is an exponential moving average (1/5/15 min). High 15-min with low current CPU + no D-state = decaying tail from a past spike. Correlate with `sar -q` for historical data. Always check CPU steal (`st%` in top) — hypervisor throttling shows as high steal with low user%.

---

## 9. LVM — Logical Volume Management

### The stack (say this order cold)

```
Physical disk / partition
        ↓  pvcreate
  Physical Volume (PV)       — raw block device initialized for LVM
        ↓  vgcreate / vgextend
   Volume Group (VG)         — pool of one or more PVs
        ↓  lvcreate
   Logical Volume (LV)       — slice of the VG, acts like a block device
        ↓  mkfs.ext4 / mkfs.xfs
      Filesystem
        ↓  mount
     Mount point
```

### Key commands

| Operation | Command |
|-----------|---------|
| Initialize a disk as PV | `pvcreate /dev/sdb` |
| Create a VG | `vgcreate data_vg /dev/sdb` |
| Add disk to existing VG | `pvcreate /dev/sdc && vgextend data_vg /dev/sdc` |
| Create an LV (fixed size) | `lvcreate -L 20G -n data_lv data_vg` |
| Create an LV (all free space) | `lvcreate -l 100%FREE -n data_lv data_vg` |
| Extend an LV + resize fs | `lvextend -r -L +10G /dev/data_vg/data_lv` |
| Extend LV only | `lvextend -L +10G /dev/data_vg/data_lv` |
| Resize ext4 fs after extend | `resize2fs /dev/data_vg/data_lv` |
| Resize XFS fs after extend | `xfs_growfs /mountpoint` |
| Snapshot | `lvcreate -s -n snap -L 5G /dev/data_vg/data_lv` |
| Revert to snapshot | `lvconvert --merge /dev/data_vg/snap` |
| Safely remove a disk | `pvmove /dev/sdb → vgreduce data_vg /dev/sdb → pvremove /dev/sdb` |
| Summary (daily use) | `pvs` / `vgs` / `lvs` |
| Verbose detail | `pvdisplay` / `vgdisplay` / `lvdisplay` |

### Critical rules — these come up in interviews

- **lvextend ≠ filesystem resize.** `lvextend` grows the block device; the filesystem is unaware. Always follow with `resize2fs` (ext4) or `xfs_growfs` (XFS). The `-r` flag on `lvextend` does both in one step.
- **XFS cannot shrink.** Period — it is architecturally grow-only. ext4 can shrink but must be unmounted first.
- **Shrink order matters: filesystem first, then LV.** `lvreduce` without shrinking the filesystem first truncates the filesystem data area → corruption. ext4 shrink: unmount → `resize2fs /dev/vg/lv <new_size>` → `lvreduce -L <new_size> /dev/vg/lv` → remount.
- **pvmove before removing a disk.** `pvmove /dev/sdb` migrates all extents off that PV to other PVs in the VG — online, no downtime. Then `vgreduce` + `pvremove`.

### Physical Extents (PE)

- Smallest unit of LVM allocation. Default: **4 MiB**.
- Set at VG creation: `vgcreate -s 8M data_vg /dev/sdb`
- LV sizes are rounded to PE boundaries.
- Max VG size = PE size × 2³². At 4 MiB PE → 16 TiB max. Increase PE size for very large VGs.

### Snapshots

- COW snapshots: only changed blocks are stored in the snapshot space.
- `-L` on snapshot = the COW buffer size. If writes fill it, the snapshot is **invalidated** (not the origin LV).
- Add write overhead to the origin LV while the snapshot is active.
- Use case: pre-migration safety net, backup of live filesystem (mount the snapshot read-only).

### One-liners worth knowing

```bash
# Show full LVM tree
lsblk

# Check VG free space
vgs --units g

# Check which PV has the most free space
pvs --units g

# Extend root LV by 10G and resize in one step (ext4 or xfs auto-detected with -r)
lvextend -r -L +10G /dev/rootvg/root

# Watch pvmove progress
pvmove /dev/sdb && watch -n1 pvs
```

---

## 10. Kernel, One Breath

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
- **Permission boundary vs SCP**: boundary = caps what permissions *one identity* can be granted (attached to a specific user/role). SCP = caps what any identity in an *OU/account* can be granted. Effective permissions = identity policy ∩ permission boundary ∩ SCP — all three must allow it.
- **Same-account vs cross-account S3 access**: same-account = identity policy OR bucket policy is sufficient. Cross-account = BOTH are required. Explicit Deny in either = denied everywhere.
- **IRSA (EKS pod identity)**: trust policy uses the cluster's OIDC provider as Federated principal + `StringEquals` condition on `sub: system:serviceaccount:<ns>:<sa>`. Without the sub condition, any pod in the cluster can assume the role — critical security gap.
- **`iam:PassRole` is a privilege escalation vector**: it lets a user attach a role to a service (EC2, Lambda, ECS). A low-priv user with PassRole on an admin role can launch a service that exercises admin permissions. Scope it: use `iam:PassedToService` condition.
- **Inline vs managed policies**: managed = reusable, versionable (10 versions), searchable in Access Analyzer. Inline = embedded in the identity, can't be reused, invisible to `aws iam list-policies`. Use inline only when you need a strict 1:1 binding.
- **MFA enforcement pattern**: `Effect: Deny, Action: *, Resource: *, Condition: {BoolIfExists: {aws:MultiFactorAuthPresent: false}}`. Denies everything when MFA is absent; BoolIfExists evaluates true when the key is missing.
- **CloudTrail for auditing**: who (`userIdentity`), what (`eventName`), where (`sourceIPAddress`), when (`eventTime`). Query: Event History (90d console), Athena on S3 trail for older/complex. CloudWatch Logs Insights if you ship trail logs there. CloudTrail ≠ CloudWatch — trail is immutable API audit, Metrics are operational telemetry.
- **Access Analyzer**: flags resources reachable by principals outside your zone of trust (account or org). Uses formal logical reasoning (Zelkova), not heuristics. External access isn't always wrong — archive findings that are intentional, fix the rest.

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
- **Task-level vs container-level resources**: task-level = Fargate allocation you pay for. Container-level = scheduling hints and soft OOM limits within that. `memory` (hard, OOM kill) vs `memoryReservation` (soft, guaranteed minimum). Container hard limits must not exceed task total.
- **`essential: true`**: when any essential container stops (crash or clean exit), ECS stops the entire task. Mark critical sidecars (log routers, Envoy) essential. Use `dependsOn` for startup ordering (START/COMPLETE/HEALTHY conditions).
- **Deploy stuck at N-1**: check `(desired × maxPercent/100) − running` for placement headroom. `minimumHealthyPercent=100, maximumPercent=100` means zero headroom — must drain before placing. Add capacity or lower minimumHealthyPercent temporarily.
- **Service Connect vs ALB**: Service Connect = east-west (service-to-service), client-side load balancing, built-in metrics, no extra hop. ALB = north-south (internet-facing ingress) or explicit internal routing with a VIP. Use Service Connect for inter-service calls inside a cluster.
- **Ephemeral volumes for inter-container sharing**: define a volume with no `host` path in `volumes` block, mount it in each container's `mountPoints`. Shared in-task, destroyed when task stops. EFS for cross-task persistence.
- **`CannotPullContainerError` — three causes**: (1) no network path to ECR (missing NAT gateway or VPC endpoint for ecr.api, ecr.dkr, s3); (2) execution role missing `ecr:GetAuthorizationToken` or `ecr:BatchGetImage`; (3) image tag doesn't exist in the repo.
- **FARGATE_SPOT**: ~70% cheaper, interruptible. 2-minute SIGTERM warning before reclamation. Design for it: graceful SIGTERM handlers, checkpointing, idempotent work. Mix with FARGATE in a capacity provider strategy using weights.
- **Secrets injection**: `secrets` block in containerDefinitions references Secrets Manager or SSM ARN. Execution role fetches at task start and injects as env var — never appears in task definition JSON. Execution role needs `secretsmanager:GetSecretValue` or `ssm:GetParameters`.
- **Blue/Green vs rolling**: Blue/Green (CodeDeploy) = two target groups, instant traffic cutover, instant rollback (one API call), keeps blue live for rollback window. Rolling = in-place batch replacement, rollback requires a new deploy of the old version. Blue/Green costs double capacity briefly but gives a clean escape hatch.
- **`healthCheckGracePeriod = 0`**: ALB checks fire immediately on task start. If app takes 15s to initialize, it fails health checks and gets replaced in a restart loop. Set grace period > cold-start time.

## 14. RDS

- **THE distinction** (guaranteed question): **Multi-AZ = availability** — synchronous standby in another AZ, automatic failover (~60–120s, DNS CNAME flips, no reads served). **Read replicas = scale** — asynchronous copies you can read from (watch ReplicaLag); cross-region capable; promotable for DR.
- **Backups**: automated daily snapshot + transaction logs → **point-in-time recovery** within the retention window; manual snapshots persist until deleted. Restores create a NEW instance (practice the DNS/endpoint swap story).
- **Parameter groups**: static params need a reboot, dynamic apply live; never modify the default group — clone it.
- **Connections**: max_connections scales with instance memory; connection storms from Lambda/ECS scale-outs → **RDS Proxy** (or PgBouncer) for pooling. Queued connections present as latency spikes with no errors (Round 2, Q12).
- **Storage perf**: gp2 **BurstBalance** hitting zero = the classic "DB suddenly slow, CPU idle" page (DiskQueueDepth climbs, read latency jumps). Fix: gp3/provisioned IOPS (online change). Long-term: alert on BurstBalance.
- **Monitoring set**: CPUUtilization, DatabaseConnections, Read/WriteLatency, DiskQueueDepth, FreeableMemory, FreeStorageSpace, ReplicaLag + **Performance Insights** for top waits/queries.
- **Aurora** (one breath): shared distributed storage across 3 AZs, replicas share storage so lag is tiny (~sub-10ms — no data to ship, just cache invalidations), failover faster (~30s vs 60-120s), reader/writer endpoints.
- **Multi-AZ failover mechanics**: standby is promoted → DNS CNAME of the existing endpoint flips to standby. App reconnects to the same hostname — no endpoint change needed. Existing connections drop; app needs retry logic + short DNS TTL. Multi-AZ standby serves ZERO traffic (not a read replica).
- **Cross-region DR**: cross-region read replicas — async replication to another region, manually promotable. Automated backups stay in-region unless explicitly copied. Multi-AZ is intra-region only.
- **PITR restore**: always creates a NEW instance with a NEW endpoint. Never modifies the source. Validate data, then update connection string or do a blue/green cutover.
- **Connection-pool queuing vs refusal**: queuing = latency spikes, error rate stays flat (requests eventually served). Refusal (max_connections hit) = immediate errors. The latency-without-errors signature is the tell.
- **Default parameter group**: read-only — clone it to a custom group, modify there, attach to instance. Static params = pending-reboot state until you explicitly reboot.

## 15. The SRE Layer (rest of the colleague's list, condensed)

- **SLI** = measurement (p99 latency, success rate). **SLO** = target (99.9% over 30d). **SLA** = contract with penalties. **Error budget** = 1 − SLO: budget left → ship features; budget burned → freeze and fix reliability. This framing turns dev-vs-ops fights into math.
- **Four golden signals**: latency, traffic, errors, **saturation**. Alert on *symptoms* (user-facing SLO burn), not causes (CPU%).
- **Toil**: manual, repetitive, automatable, scales with growth — the job is eliminating it.
- **Incident flow**: ack → severity → IC + comms roles → **mitigate first** (rollback/failover/scale), root-cause later → blameless postmortem with owned action items. Metrics: MTTD/MTTR.
- **New Relic**: APM transactions/distributed traces, NRQL shape — `SELECT percentile(duration, 99) FROM Transaction WHERE appName='X' FACET host TIMESERIES` — alert conditions tied to SLOs.

## 15c. PagerDuty — On-Call Best Practices

### Core concepts

- **Acknowledge** = "I'm looking at it." Stops escalation and repeat notifications. Incident stays **open**.
- **Resolve** = "It's fixed." Closes the incident, stops all notifications.
- Confusing the two is a common mistake: acking without resolving leaves ghost incidents; resolving without fixing kills the safety net.

### Escalation policies

- Define a **sequence of targets** (user / schedule / team) with a **timeout** per level.
- If no ack within the Level 1 timeout → PD automatically notifies Level 2. No human intervention required.
- Best practice: minimum 2 levels. Level 1 = on-call engineer. Level 2 = senior / team lead. Level 3 = manager (for prolonged SEV1s).
- **Repeat notifications**: configure PD to re-notify the same person every N minutes within a level before escalating — catches missed vibrations.

### On-call schedules

- **Weekly rotations** are standard. Overlap by a day for handoff.
- **Handoff checklist**: outgoing on-call documents open incidents, known flappy alerts, and anything mid-investigation.
- **Overrides**: for planned leave — add manually or via API. Never let a rotation go uncovered.
- **Follow-the-sun**: for global teams, route pages to whoever is in business hours. Reduces wake-ups.

### Alert fatigue — the biggest on-call risk

Alert fatigue = high page volume causes engineers to tune out → real incident gets ignored.

**Signs you have it:** > 5 pages/night average, alerts that auto-resolve before the engineer can act, engineers silencing their phones.

**The SRE fix:**
- Every alert must be **actionable** (you know what to do), **urgent** (can't wait until morning), and **symptom-based** (alert on user impact, not CPU%).
- Any alert that fires and auto-resolves within minutes = delete or tune the threshold.
- Any alert with no runbook = fix it before the next rotation.
- Track alert volume weekly per service. Treat noisy alerts as bugs.

### Severity levels (common convention)

| Sev | Impact | Response |
|-----|--------|----------|
| SEV1 / P1 | Production down, data loss, revenue impact | All hands, IC assigned, war room immediately |
| SEV2 / P2 | Significant degradation, core feature broken | On-call + lead, active mitigation |
| SEV3 / P3 | Minor impact, workaround available | Fix within business hours |
| SEV4 / P4 | Cosmetic, no user impact | Ticket, fix in next sprint |

### SEV1 incident response order (say this cold)

1. **Acknowledge** the page
2. **Assess severity** — is this actually SEV1?
3. **Open a bridge** (Slack war room / Zoom) — assign an **Incident Commander** to own communication
4. **Mitigate first** — rollback, failover, scale, kill the bad job. Root cause comes later.
5. **Communicate status** to stakeholders every 15–30 min (status page + Slack)
6. Once stable: **root cause analysis**
7. **Resolve** in PagerDuty
8. **Blameless postmortem** with owned action items and due dates

> "Mitigate first, root cause later" — the core SRE incident principle.

### Runbook requirements (every actionable alert needs one)

- What is this alert and what does it measure?
- What is the first thing to check?
- How do you mitigate it?
- When and how do you escalate?
- Link to relevant dashboards / logs

### PagerDuty architecture

- **Service**: logical grouping of related alerts (e.g. "payments-api"). Has its own escalation policy.
- **Integration**: how alerts arrive — CloudWatch, Datadog, New Relic, custom webhook via Events API.
- **Event rules / routing**: filter/deduplicate/suppress before creating incidents. Suppress known maintenance windows here.
- **Stakeholder licenses**: notify non-technical stakeholders (PMs, executives) without giving them full PD access.

## 15b. CIDR & Subnetting

**The formula — memorize this cold:**
> Usable hosts = **2^(32 − prefix) − 2**
> (subtract 2 for network address and broadcast address)

**Common prefix lengths:**

| Prefix | Host bits | Total IPs | Usable hosts | Subnet mask |
|--------|-----------|-----------|--------------|-------------|
| /20 | 12 | 4,096 | **4,094** | 255.255.240.0 |
| /21 | 11 | 2,048 | 2,046 | 255.255.248.0 |
| /22 | 10 | 1,024 | **1,022** | 255.255.252.0 |
| /23 | 9 | 512 | 510 | 255.255.254.0 |
| /24 | 8 | 256 | **254** | 255.255.255.0 |
| /25 | 7 | 128 | 126 | 255.255.255.128 |
| /26 | 6 | 64 | 62 | 255.255.255.192 |
| /27 | 5 | 32 | **30** | 255.255.255.224 |
| /28 | 4 | 16 | **11*** | 255.255.255.240 |
| /32 | 0 | 1 | 1 (single host) | 255.255.255.255 |

*AWS reserves 5 IPs per subnet (network, router, DNS, reserved, broadcast) → /28 gives 11 usable.

**RFC 1918 private ranges (memorize all three):**
- `10.0.0.0/8` — 16.7M addresses (Class A)
- `172.16.0.0/12` — covers 172.16.x.x–172.31.x.x, 1M addresses (Class B)
- `192.168.0.0/16` — 65K addresses (Class C)

**Special ranges:**
- `0.0.0.0/0` — default route, matches all IPs
- `127.0.0.0/8` — loopback
- `169.254.0.0/16` — link-local (APIPA / AWS IMDS at 169.254.169.254 / VPC DNS at 169.254.169.253)
- `/32` — single host route

**Key operations:**

*Network address*: AND the IP with the subnet mask.
> 192.168.5.130 & 255.255.255.128 (/25) = 192.168.5.128

*Broadcast address*: OR the network address with the inverted mask.
> 192.168.5.128 | 0.0.0.127 = 192.168.5.255

*How many /24s fit in a /16?*: 2^(24−16) = 256

*Aggregation*: two blocks aggregate only when contiguous AND aligned to the summary boundary.
> 10.0.0.0/24 + 10.0.1.0/24 → 10.0.0.0/23 (differ only in bit 23)

**AWS-specific:**
- VPC CIDR: /16 (max) to /28 (min)
- AWS reserves 5 IPs per subnet: .0 (network), .1 (VPC router), .2 (DNS), .3 (future), last (broadcast)
- Typical pattern: /16 VPC → /24 subnets per tier per AZ — uses 9 of 256 available /24 blocks in a /16, leaves room to grow
- Secondary CIDRs: up to 4 additional blocks can be added to a VPC

**Quick mental math shortcuts:**
- /22 boundary repeats every **4** in the third octet (4.0, 8.0, 12.0…)
- /23 boundary repeats every **2** in the third octet
- /25 splits a /24 in half at **.128**
- /26 splits into quarters at **.0, .64, .128, .192**
- /27 splits into eighths at **.0, .32, .64, .96, .128, .160, .192, .224**

---
---

# Part 3 — Containers & Kubernetes

## 16. Containers Under the Hood

**The answer:** a container is **not a VM** — it's just a Linux process (tree) wrapped in three kernel features:
1. **Namespaces** = what the process can *see* (pid, net, mnt, uts, ipc, user — its own PID 1, its own network stack, its own filesystem view)
2. **cgroups** = what the process can *use* (CPU, memory, pids limits — the exit-137 machinery from Part 1)
3. **Layered filesystem** (overlayfs) = image layers stacked read-only + a writable layer on top

Say it like: *"A container is a process with namespaces for isolation, cgroups for limits, and an overlay filesystem — same kernel as the host, which is why `ps` on the host shows every container's processes."*

**Direct Part 1 tie-ins (interviewers chain these deliberately):**
- Container memory limit = cgroup limit → exceed it → **OOMKilled, exit 137** — host free RAM irrelevant.
- **Your app is PID 1** → it inherits init's reaping duty → apps that fork but don't reap fill the container with zombies. Fix: `docker run --init` / ECS `initProcessEnabled` (injects **tini**).
- Stop sequence: SIGTERM → grace period → SIGKILL. PID 1 has no default SIGTERM handler — another reason tini exists.
- Image vs container: image = read-only layers; container = running process + ephemeral writable layer. **Writable layer is destroyed on `docker rm`** — never store persistent data there.

**Multi-stage builds (mid-senior signal):**
- Single-stage: build tools (gcc, npm, go toolchain) end up in the production image → bloated (~1GB), large attack surface.
- Multi-stage: compile in a `FROM golang:1.22 AS builder` stage, copy only the binary into `FROM scratch` or `FROM alpine`. Result: ~10MB final image, no compiler, no shell.
- Layer caching: Docker cache is ordered and content-addressed — a cache miss invalidates all subsequent layers. **Order instructions least-changed → most-changed**: copy dependency manifests first, install deps (cached), then copy source (busts cache only when code changes).

**Container security — the questions that expose gaps:**
- `securityContext.privileged: true` ≈ root on the host. Grants nearly all Linux capabilities, access to all host devices, ability to mount filesystems and load kernel modules. A compromised privileged container = compromised node.
- Principle of least privilege: add only specific capabilities needed (e.g., `NET_ADMIN`). Use `securityContext.runAsNonRoot: true` + `USER 1000` in the Dockerfile.
- **Pod Security Admission (PSA)** (replaced PodSecurityPolicy in 1.25): `restricted` profile enforces non-root, no privilege escalation, read-only root filesystem at the namespace level.

**Pod network model:**
- All containers in a pod share **one network namespace** (same IP, same `lo`). They talk to each other on `localhost:<port>`. The **pause/infra container** holds the namespace open — app containers attach to it.
- Port conflicts between same-pod containers are real. Containers in different pods must use Services.

## 17. Kubernetes Core

**Architecture in one breath:** Control plane = **API server** (front door, everything goes through it), **etcd** (the database — lose it, lose the cluster), **scheduler** (picks nodes), **controller-manager** (reconciliation loops). Each node runs **kubelet** (starts/monitors pods), a container runtime (containerd), and **kube-proxy** (programs Service routing via iptables/IPVS).

**The mental model that marks a senior:** Kubernetes is a **reconciliation engine** — you declare desired state in etcd, controllers loop forever making reality match.

**Workload controllers:**
- **Deployment → ReplicaSet → Pods**: manages rollouts. `kubectl rollout undo` reactivates the previous RS. Tune with `maxSurge`/`maxUnavailable`. Rolling update stalls if new pods never become Ready — old pods are preserved (safe but stuck).
- **StatefulSet**: use when pods need *stable identity*. Guarantees: (1) stable hostnames `pod-0`, `pod-1` via headless Service (`clusterIP: None`), (2) dedicated PVC per pod via `volumeClaimTemplates` (survives rescheduling), (3) ordered start/stop. Use for: Kafka, Cassandra, Redis Cluster, Postgres with streaming replicas.
- **DaemonSet**: one pod per node (or per matching node). Use for node agents: log shippers, monitoring exporters, CNI plugins.
- **Job / CronJob**: run-to-completion workloads.

**Services & networking:**
- **Service types:** ClusterIP (internal virtual IP, default), NodePort (exposes on every node's IP:port), LoadBalancer (provisions cloud LB), ExternalName (CNAME alias).
- DNS: `svc.namespace.svc.cluster.local` via CoreDNS. `ndots:5` causes up to 5 DNS queries per lookup on short names — reduce to `ndots:2` for services that resolve many external names.
- **Ingress**: L7 routing (host/path-based) in front of Services. Needs an Ingress Controller (nginx, AWS ALB, Traefik).
- **NetworkPolicy**: selects pods and specifies allowed ingress/egress. Default = allow all. Once a NetworkPolicy selects a pod → deny all non-matching traffic. Enforced by the CNI (Calico, Cilium); Flannel ignores it. Default-deny pattern: apply a catch-all empty-spec NetworkPolicy to every namespace, then add specific allow rules.

**Probes — know the difference cold:**
- **Liveness** fails → kubelet **RESTARTS** the container. Only check "am I deadlocked?" Never check "is my DB up?" — that cascades into cluster-wide restart storms.
- **Readiness** fails → pod **REMOVED from Service endpoints** (no restart). Check "am I ready to serve traffic?"
- **Startup** → holds off liveness/readiness for slow-booting apps.

**Requests vs limits:**
- **Requests** = scheduler reservation (placement math). **Limits** = cgroup enforcement.
- CPU over limit → **throttled** (silent, just slower). Memory over limit → **OOMKilled** (exit 137).
- **QoS classes** (auto-assigned): **Guaranteed** (requests == limits, both set) → **Burstable** (requests < limits) → **BestEffort** (nothing set). Eviction order under node memory pressure: BestEffort first, Burstable next, Guaranteed last.
- JVM trap: JVM reads `/proc/meminfo` (host RAM) and sizes heap from that → heap exceeds cgroup limit → immediate OOMKill. Fix: `-XX:MaxRAMPercentage` with `UseContainerSupport` (JDK 8u191+).

**Storage:**
- **PersistentVolume (PV)**: a cluster-level storage resource (admin-created or dynamically provisioned).
- **PersistentVolumeClaim (PVC)**: a pod's request for storage (developer-created). Bound to a PV.
- **StorageClass**: provisioner policy. Dev creates PVC → SC's provisioner auto-creates PV (e.g., EBS `gp3`). Reclaim policies: `Delete` (PV deleted with PVC, default for dynamic), `Retain` (PV kept for manual recovery).
- Access modes: `ReadWriteOnce` (single node, EBS), `ReadWriteMany` (multiple nodes simultaneously, EFS/NFS).
- **emptyDir**: ephemeral volume shared by containers in the same pod — lives and dies with the pod.

**Scheduling controls:**
- **Taints & tolerations**: node taint repels pods; pod toleration overrides a specific taint. Effects: `NoSchedule` (won't schedule new pods), `PreferNoSchedule` (soft), `NoExecute` (evicts existing pods without toleration). Use to dedicate nodes (GPU, high-memory) or quarantine sick nodes.
- **Node/pod affinity**: `requiredDuringScheduling...` = hard constraint (pod stays Pending if unmet). `preferredDuringScheduling...` = soft. **Pod anti-affinity** with `topologyKey: topology.kubernetes.io/zone` = spread replicas across AZs for HA.
- **LimitRange**: sets namespace-level default requests/limits so pods can't omit them (prevents BestEffort QoS).
- **ResourceQuota**: caps total resource consumption per namespace.

**Autoscaling:**
- **HPA** (Horizontal Pod Autoscaler): scales replica count on metrics. Default: CPU utilization (requires `metrics-server`). Custom/external metrics (RPS, queue depth): Prometheus Adapter or KEDA. Cooldown: `scaleDown.stabilizationWindowSeconds` (default 5min) prevents thrashing. Never manually set replicas on an HPA-managed Deployment.
- **Cluster Autoscaler (CA)**: adds/removes nodes when pods are unschedulable or nodes are underutilized. Works with HPA: HPA creates pods → CA adds nodes to fit them.
- **PDB** (PodDisruptionBudget): limits voluntary disruptions. `minAvailable: 2` → `kubectl drain` blocks if eviction would drop below 2. Doesn't protect against node crashes. Never set `minAvailable == replicas` — blocks all drains.

**Multi-container patterns:**
- **Sidecar**: augments the main container without changing it. Examples: log shipper, service mesh proxy (Envoy), secret refresher.
- **Init container**: runs to completion *before* app containers start. Use for: wait-for-DB-ready, run migrations, populate shared volumes with config/certs.
- **Ambassador**: proxies outbound traffic (e.g., Envoy for circuit-breaking, retries, mTLS).

**RBAC:**
- `Role` = namespace-scoped; `ClusterRole` = cluster-scoped. Use ClusterRole for: cluster-scoped resources (nodes, PVs), cross-namespace permissions, or non-namespaced API groups.
- `RoleBinding` attaches to a namespace; `ClusterRoleBinding` attaches cluster-wide. Prefer RoleBinding over ClusterRoleBinding — least privilege.
- Audit: `kubectl get clusterrolebindings | grep default` often reveals over-permissioned service accounts.

**Helm:**
- Chart = templated K8s manifests + `values.yaml`. Release = a named instance of a chart deployed to a cluster.
- `helm upgrade --install` = upsert (create if not exists, upgrade otherwise) — idempotent, safe for CI/CD.
- Failed upgrade: release state = `failed`, previous revision still active. Roll back: `helm rollback <release> <rev>`. Use `--atomic` flag in pipelines (auto-rollback on failure).

**EKS specifics:**
- Managed control plane (AWS handles etcd, API server HA). Node groups (EC2) or Fargate profiles.
- **IRSA** (IAM Roles for Service Accounts): annotate a K8s SA with an IAM role ARN → pods get scoped temporary AWS creds via projected tokens. Never use node-level IAM roles for workload permissions.

## 18. K8s Debugging Playbook

**Universal first moves:** `kubectl describe pod X` (Events section has the answer 80% of the time), `kubectl logs X --previous` (crashed container's logs, not the new instance), `kubectl get events --sort-by=.lastTimestamp -A`.

| Symptom | Root cause | First commands |
|---|---|---|
| **CrashLoopBackOff** | App exits immediately; backoff doubles | `logs --previous`; check exit code: 1=app, **137=OOM/kill**, 143=SIGTERM |
| **OOMKilled** | Memory limit too low, or JVM read host RAM, or leak | `describe pod` → reason: OOMKilled; `kubectl top pod`; set `-XX:MaxRAMPercentage` for JVM |
| **ImagePullBackOff** | Bad tag / missing imagePullSecret / ECR IAM missing | `describe pod` Events; verify ECR node IAM role; check imagePullSecrets |
| **Pending** | Scheduler can't place: capacity, taints, affinity, unbound PVC | `describe pod` → "0/N nodes available: …" tells exact reason |
| **Readiness failing** | Wrong port, slow warmup, dependency check in probe | `describe pod` + `kubectl get endpoints <svc>` |
| **Node NotReady** | kubelet down, disk/memory/pid pressure, network partition | `describe node` Conditions; SSH → Part 1 territory: D-states, OOM, full disk |
| **Evictions** | Node memory/disk pressure; BestEffort/Burstable evicted first | `kubectl get events`; add honest requests/limits; LimitRange enforcement |
| **Stuck Terminating** | preStop hook hung, SIGTERM ignored, node offline | `--force --grace-period=0` removes from etcd; surviving container processes orphaned if node returns |
| **Drain hangs** | PDB violation blocking eviction, or unowned (orphan) pods | `kubectl describe pdb`; add `--delete-emptydir-data --ignore-daemonsets` flags |
| **Rolling update stalled** | New pods Running but Readiness failing — old pods preserved | `describe pod <new>` for probe failures; check config, ports, dependencies |
| **DNS resolution fails** | CoreDNS down, wrong search domain, ndots misconfiguration | `exec` into pod → `cat /etc/resolv.conf`; `nslookup kubernetes.default`; `kubectl get pods -n kube-system -l k8s-app=kube-dns` |

**DNS gotcha:** `ndots:5` means a lookup of `redis` triggers queries for `redis.namespace.svc.cluster.local`, `redis.svc.cluster.local`, `redis.cluster.local`, `redis.search.domain`, and finally `redis.` — 5 round trips before resolving. Set `dnsConfig.options: [{name: ndots, value: "2"}]` for services that do heavy external DNS lookups.

**The closing line for any K8s debugging answer:** "...and if the node itself is sick, I'm back to Linux fundamentals — kubelet logs, dmesg, D-states, disk pressure. Kubernetes is a scheduler on top of everything in Part 1."

## Top 5 Kubernetes Commands (Daily Use)

**1. `kubectl get` — see what's running**
```bash
kubectl get pods                          # all pods in default namespace
kubectl get pods -n kube-system           # specific namespace
kubectl get pods -A                       # all namespaces
kubectl get pods -o wide                  # add node/IP columns
kubectl get all                           # pods, services, deployments, etc.
```

**2. `kubectl describe` — diagnose why something is broken**
```bash
kubectl describe pod <name>               # events, resource limits, restart reason
kubectl describe node <name>             # capacity, conditions, running pods
kubectl describe deployment <name>       # rollout status, replica counts
```
The `Events:` section at the bottom is almost always where the answer is.

**3. `kubectl logs` — read output**
```bash
kubectl logs <pod>                        # current logs
kubectl logs <pod> -f                     # follow (tail -f equivalent)
kubectl logs <pod> --previous            # logs from the crashed previous container
kubectl logs <pod> -c <container>        # specific container in a multi-container pod
kubectl logs -l app=api --tail=100       # logs from all pods matching a label
```

**4. `kubectl exec` — get inside a running container**
```bash
kubectl exec -it <pod> -- bash            # interactive shell
kubectl exec -it <pod> -- sh              # if no bash (alpine images)
kubectl exec <pod> -- env                 # dump env vars without interactive
kubectl exec -it <pod> -c <sidecar> -- sh  # specific container
```

**5. `kubectl rollout` — manage deployments**
```bash
kubectl rollout status deployment/<name>  # watch a deploy complete
kubectl rollout history deployment/<name> # see revision history
kubectl rollout undo deployment/<name>    # roll back to previous revision
kubectl rollout restart deployment/<name> # rolling restart (recycles pods gracefully)
```

**Bonus — find recently crashed pods fast:**
```bash
kubectl get pods --sort-by='.status.startTime' | tail -20
```

---
---

# Part 4 — Identity Management & PKI

## 19. Identity: OAuth 2.0, OIDC, SAML, LDAP, Okta/OneLogin

### The One-Sentence Separator (say this first every time)
- **OAuth 2.0** = *authorization* — "what can this app access on my behalf?"
- **OIDC (OpenID Connect)** = *authentication* — "who is this user?" (OAuth 2.0 + identity layer)
- **SAML 2.0** = *both auth + authz in one XML assertion* — older enterprise standard, federation-first
- **LDAP** = *directory protocol* — query/modify a directory (users, groups, attributes) over the network

---

### OAuth 2.0 — The Mental Model

**Four roles:**
| Role | What it is |
|---|---|
| Resource Owner | The user granting access |
| Client | The application requesting access |
| Authorization Server | Issues tokens after consent (Okta, Cognito, Auth0) |
| Resource Server | The API holding the protected data |

**Grant types — know these cold:**

| Grant | Use case | Notes |
|---|---|---|
| **Authorization Code + PKCE** | Web apps, SPAs, mobile | **Modern default for all clients.** PKCE replaces client secret for public clients |
| **Client Credentials** | Machine-to-machine (M2M) | No user involved — service accounts, crons, backend APIs |
| **Device Authorization** | Smart TVs, CLIs | User approves on secondary device; device polls for token |
| ~~Implicit~~ | ~~SPAs (old)~~ | **Obsolete.** Tokens leaked in URL fragment. Removed in OAuth 2.1 |
| ~~Resource Owner Password~~ | ~~Legacy~~ | **Never use.** Client sees user's password, breaks MFA/SSO |

**PKCE mechanics (code_verifier / code_challenge):**
1. Client generates a random `code_verifier`, hashes it → `code_challenge` (SHA-256)
2. Sends `code_challenge` with the auth request
3. Auth server stores it; after redirect, client proves it holds the original `code_verifier`
4. Even if the auth code is intercepted, attacker can't exchange it without `code_verifier`

**Token types:**
- **Access token** — short-lived (5–60 min), presented to Resource Server. Opaque (server must call `/introspect`) or **JWT** (self-contained, validated locally with the public key — no introspect call needed, but can't be revoked until expiry)
- **Refresh token** — long-lived, stored securely server-side. Exchanges for new access tokens. Use **refresh token rotation** (invalidate on use) to detect theft
- **ID token** — OIDC only. JWT containing user identity claims (sub, email, name). Client reads it; never send it to an API

**Security pitfalls (senior-filter answers):**
- **state parameter** = random nonce tied to the session. Prevents CSRF on the redirect callback. Server must verify it matches what was sent
- **redirect_uri** must be pre-registered and matched exactly (not prefix) — prevents open-redirector attacks
- **Never store tokens in localStorage** — XSS-accessible. Use in-memory or `HttpOnly` cookies via a backend-for-frontend (BFF)
- **JWT algorithm confusion** — always whitelist `alg` on your server; never trust the `alg` header. `alg: none` attack strips signature

**OAuth 2.1 changes (know this for 2026 interviews):** PKCE required for all auth-code flows, Implicit removed, Resource Owner Password removed, bearer tokens banned from query strings.

---

### OIDC — What it adds

- Standardises the **ID token** (JWT with `sub`, `iss`, `aud`, `exp`, `iat`, `nonce`)
- Adds the **UserInfo endpoint** (`GET /userinfo` with access token → extra profile claims)
- Adds **discovery** (`/.well-known/openid-configuration`) so clients auto-configure endpoints and keys
- **Nonce** in auth request → ID token; prevents replay attacks
- **ACR / AMR claims** — `acr` = authentication class reference (strength level); `amr` = methods used (password, mfa, hwk). Use `acr_values` in the auth request to demand MFA

---

### SAML 2.0 — Enterprise SSO

**Flow (SP-initiated, most common):**
1. User hits Service Provider (SP) — e.g. Salesforce, AWS console, a SaaS app
2. SP redirects to Identity Provider (IdP) — Okta, OneLogin, AD FS
3. User authenticates at IdP
4. IdP issues a signed **SAML Assertion** (XML) → posted back to SP's Assertion Consumer Service (ACS) URL
5. SP validates signature, extracts attributes, creates session

**Key concepts:**
- **Assertion** = the signed XML blob containing: authentication statement (who), attribute statement (email, groups, roles), authorisation decision
- **Metadata exchange** — SP and IdP share an XML metadata file containing certificates, endpoints, and entity IDs. This is how trust is established (import each other's metadata)
- **SP-initiated vs IdP-initiated** — SP-initiated is more secure (SP generates a signed AuthnRequest with a random ID it can verify). IdP-initiated has no AuthnRequest to validate — CSRF risk
- **Signature validation** — SP must verify the assertion signature against the IdP's signing certificate. If you skip this, anyone can forge an assertion

**SAML vs OIDC — when to choose:**
| | SAML | OIDC |
|---|---|---|
| Format | XML | JSON/JWT |
| Age | 2005, enterprise-legacy | 2014, modern |
| Best for | Legacy enterprise apps, Windows/AD ecosystems | New apps, APIs, mobile, microservices |
| Token format | Assertions (XML, large) | JWTs (compact) |
| Okta/OneLogin | Support both | Prefer OIDC for new integrations |

---

### Okta & OneLogin — SRE Operations Layer

Both are cloud IdPs that act as the authoritative **identity broker** — they federate upstream directories (AD, LDAP, HR systems) and issue SAML/OIDC tokens downstream to apps.

**Key concepts for SRE interviews:**
- **Universal Directory / LDAP Interface** — Okta/OneLogin can masquerade as an LDAP endpoint for legacy apps that only speak LDAP
- **SCIM (System for Cross-domain Identity Management)** — REST API standard for automating **user provisioning and deprovisioning**. When HR adds/removes a user in Workday → SCIM pushes to Okta → Okta SCIM-provisions the user in downstream apps. Without SCIM: manual offboarding = security risk
- **Just-In-Time (JIT) Provisioning** — create the user account in the SP on first successful SAML login, using attributes from the assertion. Simpler than SCIM but can't deprovision
- **MFA Policies** — step-up auth (require MFA for sensitive apps), adaptive MFA (risk-based: new device/country triggers MFA), phishing-resistant (FIDO2/WebAuthn hardware keys)
- **Lifecycle Management** — automate joiners/movers/leavers. Leaver flow: HR terminates → Okta suspends (not deletes) → all app sessions revoked via OIDC back-channel logout or SAML SLO
- **Okta FastPass / Device Trust** — require managed device certificate before granting access. Ties into MDM (Jamf, Intune)

**Debugging SSO failures (senior-filter answers):**
- SAML assertion failures: use a browser SAML tracer extension to decode the base64 assertion; check `NotBefore`/`NotOnOrAfter` (clock skew!), audience restriction, and signature
- `403 Access Denied` after successful SSO: user authenticated but SP's attribute-based authz failed — check group/role attributes in the assertion
- Session loops: SP doesn't trust the assertion, keeps redirecting back to IdP — usually a certificate mismatch or wrong ACS URL

---

### LDAP — Directory Protocol

**Structure:**
- **DIT (Directory Information Tree)** — hierarchical tree of entries
- **DN (Distinguished Name)** — unique path to an entry: `CN=John Smith,OU=Engineering,DC=example,DC=com`
  - `CN` = Common Name, `OU` = Organizational Unit, `DC` = Domain Component
- **Attributes** — each entry has typed attributes: `mail`, `uid`, `memberOf`, `userPassword`, `objectClass`
- **objectClass** — defines what attributes an entry can/must have (schema enforcement)

**Operations:** bind (authenticate), search (query), add, modify, delete, compare, unbind

**Ports:**
- `389` — LDAP (cleartext/StartTLS)
- `636` — LDAPS (TLS from the start)
- `3268` / `3269` — AD Global Catalog (plain / TLS)

**Bind types:**
- **Simple bind** — DN + password in cleartext → always use StartTLS or LDAPS
- **SASL bind** — pluggable auth (Kerberos/GSSAPI most common in AD environments)
- **Anonymous bind** — no credentials; most directories restrict what anonymous can read

**Common LDAP in SRE contexts:**
- App authenticates users against LDAP: `bind as service account → search for user by uid → bind as user with provided password → success/fail`
- **Service account (bind DN)** — read-only account the app uses to search the directory. Rotate its password → update everywhere or you get `49 Invalid Credentials` floods
- **LDAP referrals** — a DC forwards the client to another server. Can cause app hangs if the client doesn't follow them (set `referrals=off` in most app configs)
- `ldapsearch` for debugging: `ldapsearch -H ldap://dc.example.com -D "CN=svc,DC=example,DC=com" -W -b "DC=example,DC=com" "(uid=jsmith)"`

**LDAP vs Active Directory:**
LDAP is the *protocol*; AD is Microsoft's *directory service* that uses LDAP as its access protocol plus Kerberos for authentication, DNS for locating DCs, and its own replication protocol.

---

## 20. PKI, TLS/SSL Certificates

### The TLS Handshake (TLS 1.3, simplified)

```
Client                          Server
  |-- ClientHello (TLS ver, ciphers, key_share) -->|
  |<-- ServerHello (chosen cipher, key_share) ------|
  |<-- Certificate (server's cert chain) ----------|
  |<-- CertificateVerify (signed with private key) |
  |<-- Finished (MAC) -----------------------------|
  |-- Finished (MAC) ------------------------------>|
  |======= Application Data (encrypted) ===========|
```

Key points:
- TLS 1.3 eliminates the RSA key exchange (no more "decrypt session key with private key") — **ephemeral Diffie-Hellman (DHE/ECDHE) only** → mandatory Perfect Forward Secrecy
- The server's private key is only used to *sign* the key exchange, not encrypt it
- **1-RTT** handshake (vs TLS 1.2's 2-RTT). **0-RTT** resumption available but replay-attack risk — don't use for non-idempotent requests

---

### Certificate Chain (Root → Intermediate → Leaf)

```
Root CA (self-signed, in browser/OS trust store)
  └── Intermediate CA (signed by Root)
        └── Leaf/Server Certificate (signed by Intermediate, issued to your domain)
```

- **Root CA** private key is kept **offline in an HSM** — never used for day-to-day signing. If a root CA is compromised, every certificate it ever issued is untrusted
- **Intermediate CA** does the actual signing. Isolates the root — a compromised intermediate can be revoked without rotating the root
- **Chain of trust** — client verifies: leaf cert signed by intermediate? Intermediate signed by root? Root in trust store? All not expired or revoked?
- **Missing intermediate** = the most common `SSL_ERROR_RX_RECORD_TOO_LONG` / `unable to verify certificate chain` error. Your server must send the full chain (leaf + intermediates, not root)

---

### Certificate Types — Validation Levels

| Type | Verification | Use case |
|---|---|---|
| **DV (Domain Validated)** | CA only confirms domain control (DNS/HTTP challenge) | Fast/free (Let's Encrypt), internal services, APIs |
| **OV (Organization Validated)** | CA verifies the org is real | Public-facing business sites |
| **EV (Extended Validation)** | Full legal/physical verification | Banks, e-commerce (green bar, largely obsolete now) |
| **Wildcard** (`*.example.com`) | One cert covers all first-level subdomains | Convenient but one compromise = all subdomains at risk |
| **SAN (Subject Alternative Name)** | One cert covers multiple explicit domains | Modern standard — Chrome ignores CN, only reads SAN |

---

### Key Concepts — Senior-Filter Answers

**SNI (Server Name Indication):**
Client sends the *target hostname* in the TLS ClientHello (before the certificate is sent), allowing one IP to host many TLS sites. Without SNI: one IP = one cert. With SNI: ALBs, nginx, Cloudflare host thousands of certs per IP. Older clients (IE on WinXP) don't support SNI — irrelevant now but good to mention.

**mTLS (Mutual TLS):**
Both sides present certificates. Server authenticates client, client authenticates server. Standard in:
- Service meshes (Istio/Linkerd) — every service-to-service call is mTLSed
- Zero-trust internal networks
- Client certificate auth for API consumers

**Perfect Forward Secrecy (PFS):**
Ephemeral key exchange (DHE/ECDHE) means each session uses a unique session key derived from a fresh DH exchange. Recording encrypted traffic today and stealing the server's private key tomorrow won't decrypt past sessions. TLS 1.3 mandates this; TLS 1.2 requires it only if you pick ephemeral cipher suites.

**OCSP & OCSP Stapling:**
- **OCSP (Online Certificate Status Protocol)** — client queries CA's OCSP responder to check if cert is revoked. Problem: adds latency, leaks browsing history to CA, OCSP responder downtime breaks things
- **OCSP Stapling** — server pre-fetches and caches a signed OCSP response, "staples" it to the TLS handshake. Client gets revocation status without contacting the CA. *Always enable OCSP stapling on internet-facing servers.*

**Certificate Pinning:**
Hardcode the expected cert or public key hash in the client. HTTPS with pinning: even if an attacker gets a valid CA-signed cert for your domain (rogue CA), the pinned client rejects it. Risk: pin the wrong cert → users locked out. Use `backup pins` and a short `max-age`.

**HSM (Hardware Security Module):**
Physical device that stores private keys and performs crypto operations. Key never leaves the HSM. Required for root CAs and high-security environments. Cloud equivalents: AWS CloudHSM, GCP Cloud HSM.

---

### Common Certificate Errors & Debugging

| Error | Cause | Fix |
|---|---|---|
| `CERTIFICATE_VERIFY_FAILED` | CA not in trust store, or self-signed | Add CA to trust store, or use a public CA |
| `SSL_ERROR_BAD_CERT_DOMAIN` / CN mismatch | Cert issued for wrong domain, or SAN missing | Reissue cert with correct SANs |
| `CERTIFICATE_EXPIRED` | `NotAfter` in the past | Renew; automate with ACME/Let's Encrypt |
| `incomplete chain` / `unable to verify` | Server not sending intermediate cert | Configure server to send full chain |
| Clock skew | `NotBefore` in the future (server time wrong) | Fix NTP sync |
| `TLSV1_ALERT_UNKNOWN_CA` (mTLS) | Client cert's CA not trusted by server | Add client CA to server's trust store (`SSLCACertificateFile`) |

**Quick debug toolkit:**
```bash
# Inspect cert chain from a live server
openssl s_client -connect example.com:443 -showcerts

# Check expiry
openssl x509 -in cert.pem -noout -dates

# Verify cert matches key
openssl x509 -noout -modulus -in cert.pem | md5sum
openssl rsa  -noout -modulus -in key.pem  | md5sum
# They must match — if they don't, the cert was signed for a different key

# Verify chain
openssl verify -CAfile chain.pem cert.pem

# Check OCSP stapling
openssl s_client -connect example.com:443 -status 2>/dev/null | grep -A 10 "OCSP Response"
```

---

### One-Liners Worth Saying Out Loud

- "OAuth 2.0 is authorization, not authentication — OIDC is the identity layer on top."
- "PKCE lets public clients do auth-code flow safely — the code_verifier proves it's the same client that started the flow, even without a client secret."
- "The implicit grant is dead — tokens in URL fragments leak via browser history and Referer headers."
- "Refresh token rotation: invalidate the old token on each use — if a stolen token is reused, you get a reuse-detection alert."
- "SAML is XML federation for enterprise apps; OIDC is JSON/JWT for everything modern."
- "SCIM handles provisioning; SSO handles authentication. You need both for a complete IAM story."
- "A missing intermediate CA is the most common TLS chain error — the server must send the full chain, not just the leaf cert."
- "PFS means recording my TLS traffic today won't decrypt it if my private key leaks tomorrow."
- "OCSP stapling eliminates the per-request CA round-trip and stops leaking your users' browsing to the CA."
- "mTLS in a service mesh means every east-west call is authenticated and encrypted, no shared secrets needed."

---
---

# Part 5 — Interview Precision Drills (Round 1 Weak Spots)

## 21. Closing the Gaps — Exact Phrasing Matters

These are the specific answers that slipped in Round 1. Each entry has the trap answer and the precise correction.

---

### Load Average: R + D Only. Never S.

**The trap:** adding S (sleeping) under pressure.

**The rule:** `load average = processes in R + processes in D`, averaged over 1, 5, and 15 minutes.

| State | Counts? | Why |
|---|---|---|
| R — running/runnable | **YES** | Actively wants CPU |
| D — uninterruptible sleep | **YES** | Blocked in kernel syscall; real resource pressure |
| S — interruptible sleep | **NO** | Waiting for an event; not competing for anything |
| Z — zombie | **NO** | Already dead; holds no resources |
| T — stopped | **NO** | Suspended; not competing |

**Load 45, CPU 92% idle** → ~45 processes in D, stuck on I/O — not CPU pressure at all.

**Say it cold:** *"Load average counts R plus D only. S doesn't count. High load with idle CPU means the load is all D-state: look at storage, not the scheduler."*

---

### Orphan vs Zombie vs Defunct — Precise Distinctions

**The trap:** calling a living, in-flight process "defunct."

| Term | Alive? | Resources | Parent | Fix |
|---|---|---|---|---|
| **Zombie (Z / defunct)** | **No** — exited | PID slot + tiny exit-status struct | Alive but hasn't called wait() | Repair/restart parent so it calls wait() |
| **Orphan** | **Yes** — still executing | Full: CPU, memory, FDs | Dead — re-parented to init/PID 1 | Nothing; init reaps it on exit |

**The mid-request scenario:** parent killed while child handles an HTTP request → child is an **orphan** (alive). It is NOT defunct. Defunct = already exited.

**Verb precision:** you don't *kill* zombies — they're dead. You **reap** them. The mechanism: fix the parent so it calls wait().

---

### 4096: Page Size FIRST, Block Size Second

**The trap:** leading with "filesystem block size."

**First sentence:** *"4096 bytes = 2^12 = 4 KiB — the memory page size."*

Second sentence: *"ext4/xfs also default their block size to 4096, aligned to the page size so each filesystem block maps to exactly one page cache entry."*

The page size is foundational: it governs demand paging, COW in fork(), the page cache, and allocator alignment. The filesystem block size matching it is a deliberate consequence.

**Demand paging (precise):** Virtual pages are mapped but NOT backed by physical RAM until first access. CPU raises a page fault → kernel allocates a frame and maps it → instruction retries. This is why fork() is cheap: COW shares all pages until a write occurs.

---

### D-State Exit Paths: All Three

**The trap:** giving only one (I/O completes).

**Recite all three every time:**
1. **I/O completes** — blocking syscall finishes normally
2. **I/O errors out** — syscall fails: storage timeout, `umount -f`, `umount -l`
3. **Reboot**

SIGKILL is **queued** (not lost, not stuck) — held in the pending signal set and delivered the instant the syscall returns. Without path 1 or 2, it waits indefinitely.

**Production NFS playbook:** `umount -f /mnt/nfs` (force) or `umount -l` (lazy detach) → pending I/O returns EIO/ESTALE → D-state processes unblock → queued SIGKILLs deliver.

---

### Signal Queueing: "Queued Until the Syscall Returns" — Not "Sent But Stuck"

**The trap:** *"sent but stuck."*

**"Stuck"** implies the signal might be lost. **"Queued"** is exact: it is held in the pending signal set, guaranteed to deliver the moment the process exits kernel space.

**Full sentence:** *"SIGKILL on a D-state process is queued — not ignored, not lost. It delivers the instant the blocking syscall returns. Without I/O completing or erroring, the queue waits indefinitely."*

---

### pid_max: systemd Raises It at Boot — Not "Modern Kernel"

**The trap:** *"modern systems default to 4194304"* or *"the kernel uses 2^22."*

**Precise chain:**
- Kernel compiled-in default: **32768 (2^15)** — still in the source today
- **systemd v243+ (~2019) writes 4194304 to `/proc/sys/kernel/pid_max` at boot** — a runtime sysctl
- 4194304 = 2^22 = PID_MAX_LIMIT — the kernel's hard ceiling on 64-bit

A box running SysVinit has pid_max = 32768 on a modern kernel. Verify on any box: `cat /proc/sys/kernel/pid_max`

---

### Second-Tier Precision — The Enumeration Traps

**Always count before answering:** *"There are three ways…"*, *"Two root causes…"* — state the count first, then enumerate all of them.

**fork() EAGAIN — three limits (no dropping):**
1. Per-user `ulimit -u` (RLIMIT_NPROC)
2. Kernel-wide `pid_max`
3. cgroup `pids.max` (systemd `TasksMax=`)

**Port space — say "16-bit field," not "goes up to 65536":**
*"The TCP/UDP header port field is 16 bits wide → 2^16 = 65536."* Mechanism first, number second.

**Hard link vs symlink — say "inode-level" vs "path-level":**
Hard link = same inode (inode-level). Symlink = file containing a path string (path-level, can dangle).

**OOM victim — oom_score, not random:**
Highest `oom_score` (memory footprint). Tune via `oom_score_adj` (-1000 = never kill).

**ENOSPC vs D-state hangs — different failure modes:**
- Disk blocks full: `df` at 100%, operations fail immediately with ENOSPC
- Inode exhaustion: `df -i` at 100%, creates fail with ENOSPC even with free blocks
- Dead mount D-state: processes **hang** indefinitely, never get an error — load spikes, `ps` shows D

**RSS vs VSZ — JVM is not leaking:**
Large VSZ (heap reserved) + small RSS (pages touched) = healthy. Alert on RSS growth over time, not VSZ magnitude.

**Process vs thread:**
Threads share the virtual address space and FD table with siblings. Processes do not. Both are scheduled by the kernel as tasks.

---

### Interview Habit Reminders

| Trap | Correction |
|---|---|
| **Dropped enumeration** | Count first: *"There are three…"* — then name all of them |
| **The graft** | Finish the direct answer before adding context |
| **The substitution** | If blanking: say *"I'm blanking"* — don't substitute an adjacent fact |
| **Cause vs mechanism** | If asked for a formula or count, give it first. War story is dessert, not the meal |
| **Verb looseness** | Zombies are **reaped** not killed. Port fields have **width** (bits) not length |

---
---

# Part 6 — System Design

## 22. System Design for SREs

System design interviews test whether you think in trade-offs, not textbook patterns. For every design decision, state: *what you chose, what you gave up, and why that's the right call for this context.* Reliability is never free — it costs complexity, latency, or money.

---

### The SRE Framing (say this first)

Before drawing boxes: *"I want to understand the reliability requirements first — what's the expected SLO, what's the cost of downtime, and are we more read-heavy or write-heavy?"*

Every design has three SRE questions underneath it:
1. **Where does it fail?** (single points of failure, blast radius)
2. **How fast do you know?** (observability, alerting latency)
3. **How fast do you recover?** (RTO, rollback path, runbook)

---

### High Availability Patterns

**The core principle:** eliminate single points of failure at every layer. Redundancy costs money; SPOFs cost incidents.

| Pattern | What it gives you | What it costs |
|---|---|---|
| **N+1 redundancy** | One failure is invisible to users | ~2× capacity cost |
| **Active-active** | Full capacity even during failure; no failover delay | State sync complexity, conflict resolution |
| **Active-passive** | Simpler state management; one source of truth | Failover delay (DNS TTL + health check interval); wasted standby capacity |
| **Multi-AZ** | Survives AZ outage; AWS/GCP/Azure default for HA | Replication latency; cross-AZ data transfer costs |
| **Multi-region** | Survives full region outage | Latency for global consistency; dramatically more expensive |

**Health checks + automatic failover:** load balancers and DNS (Route 53 health checks) must detect unhealthy instances and remove them from rotation faster than your error budget allows. Tune: check interval, unhealthy threshold, response timeout.

---

### Rate Limiting

**Why it matters:** without rate limiting, one misbehaving client (or a DDoS) can take down your entire service.

**Algorithms:**

| Algorithm | How it works | Best for |
|---|---|---|
| **Token bucket** | Bucket refills at fixed rate; request costs a token. Allows burst up to bucket capacity | APIs that should allow short bursts (default choice) |
| **Leaky bucket** | Requests drain at fixed rate regardless of arrival pattern | Smoothing traffic for downstream services with no burst tolerance |
| **Fixed window counter** | Count reqs per time window (e.g., per minute). Simple but boundary-exploit: 200 reqs in the last second of minute 1 + 200 in first second of minute 2 = 400 in 2 seconds | Simple cases where boundary gaming isn't a concern |
| **Sliding window log** | Exact timestamps per request; count reqs in the rolling last N seconds | Precise rate enforcement; high memory cost per user |
| **Sliding window counter** | Approximate hybrid of fixed window + previous window weighting | Best trade-off at scale: ~0.003% error rate vs exact, far less memory than log |

**Distributed rate limiting problem:** rate limit state must be shared across all gateway instances.
- **Centralized counter (Redis):** consistent but adds ~1ms network hop per request; Redis becomes a bottleneck + SPOF
- **Local + async sync:** each instance tracks its own count, syncs periodically. Allows temporary over-admission (~15% at low sync frequency) — acceptable for most APIs

**Where to enforce:** API gateway (coarse, protects infra), then per-service (finer, protects individual microservices). Two layers beats one.

---

### Caching

**The golden rule:** cache is a performance optimization, not a source of truth. Always ask: *what is the read/write ratio, how stale is tolerable, and what breaks if the cache is wrong?*

**Write strategies:**

| Strategy | How it works | Durability | Write latency |
|---|---|---|---|
| **Write-through** | Write hits cache AND storage synchronously; cache always consistent | High (data in storage immediately) | Slow (two writes) |
| **Write-back (write-behind)** | Write to cache only; flush to storage asynchronously | Risk of data loss on cache failure | Fast (one write) |
| **Write-around** | Write bypasses cache, goes directly to storage; cache populated on next read | High | Fast; cold cache on first read |

**Read strategies:**
- **Cache-aside (lazy loading):** app checks cache → on miss, reads storage, populates cache. Most common. Cache only contains what was actually read.
- **Read-through:** cache layer handles the miss automatically. Simpler app code; first request always slow.

**Eviction policies:**
- **LRU (Least Recently Used):** evict the item not accessed for the longest time. Works well for temporal locality.
- **LFU (Least Frequently Used):** evict least accessed over time. Better for stable popular items.
- **TTL:** expire after a fixed time regardless of access. Simple; prevents stale data accumulation.

**The thundering herd problem:** cache entry expires → thousands of requests simultaneously hit the database before any response can repopulate the cache.
- **Solutions:** probabilistic early expiration (re-cache slightly before TTL expires, probabilistically), mutex lock on cache miss (only one request fetches; others wait), background refresh, jittered TTLs

**Consistent hashing:** for distributed caches, consistent hashing minimises key redistribution when nodes are added/removed — only `k/n` keys move (vs all keys with modulo hashing).

---

### Circuit Breakers

**Why:** without circuit breakers, a slow or failing dependency causes your service to accumulate slow threads, exhaust connection pools, and cascade the failure upstream.

**Three states:**

```
CLOSED (normal) ──→ failure rate > threshold ──→ OPEN (fail fast)
                                                      │
                                        after timeout ↓
                                               HALF-OPEN (test)
                                          ┌── test request fails ──→ OPEN
                                          └── test request succeeds ──→ CLOSED
```

| State | What happens | Transitions |
|---|---|---|
| **Closed** | Requests flow normally; failures counted | → Open when error rate exceeds threshold (e.g., 50% over 10s) |
| **Open** | Fail fast — requests return error immediately, no call to dependency | → Half-open after a configured timeout (e.g., 30s) |
| **Half-open** | Allow one test request through | → Closed if it succeeds; → Open if it fails |

**SRE value:** circuit breakers turn a cascading failure into a fast, bounded failure. Callers get immediate errors (400ms) instead of hanging (30s timeout), freeing threads.

**Fallback strategies:** return cached/stale data, return a default response, shed the feature entirely. Match the fallback to what users can tolerate.

---

### Database Scaling

**Decision tree — apply in order:**

1. **Index and query-optimize first** — most "slow database" problems are missing indexes or N+1 queries, not capacity
2. **Vertical scale** — fastest, easiest, but has a ceiling and requires downtime on some engines
3. **Read replicas** — for read-heavy workloads (>80% reads). Async replication → eventual consistency on reads. Can't help write throughput.
4. **Connection pooling (PgBouncer, RDS Proxy)** — essential before any horizontal scaling when Lambda/ECS are involved
5. **Caching layer (Redis, Memcached)** — offload hot reads entirely from the DB
6. **Sharding** — for write-heavy workloads or datasets too large for one box. High complexity: cross-shard queries, rebalancing, hotspots. Last resort.
7. **OLAP separation** — heavy analytics queries don't belong in your OLTP database. Route to a data warehouse (Redshift, BigQuery) or read replica with ETL.

**Sharding key selection:** choose a key with high cardinality and even distribution. User ID is common. Avoid time-based keys (all writes go to the "latest" shard — a hotspot).

**The 2-billion-row table problem:** before sharding, diagnose: look at the query execution plan (`EXPLAIN ANALYZE`), identify the bottleneck (full table scan, lock contention, I/O), then consider: partitioning by range/list (less complexity than sharding, handled by the DB), archiving old data, or denormalisation.

---

### Message Queues & Delivery Semantics

**Three delivery guarantees:**

| Guarantee | How | When to use |
|---|---|---|
| **At-most-once** | Fire and forget; no retry | Logging, telemetry — losing a message is acceptable |
| **At-least-once** | Retry until acknowledged; may deliver duplicates | Default for most work queues; **requires idempotent consumers** |
| **Exactly-once** | Transactional producer + idempotent consumer + dedup. High overhead | Payments, inventory — duplicates are business-critical |

**Idempotency is the key insight:** if your consumer is idempotent (processing the same message twice has no extra effect), then at-least-once delivery is effectively exactly-once. Design consumers to be idempotent first; exactly-once infrastructure second.

**Kafka-specific:** partitions provide ordering (within a partition, not across). Consumer groups allow horizontal scaling of consumers — each partition assigned to one consumer in the group. On rebalance (consumer added/removed), partitions are reassigned — in-flight messages must be handled.

**Dead letter queue (DLQ):** messages that fail processing N times go to a DLQ for manual investigation. Without a DLQ, poison pill messages block the entire queue indefinitely.

---

### Deployment Patterns

| Pattern | Traffic cutover | Rollback | Extra infra | Best for |
|---|---|---|---|---|
| **Rolling** | Gradual (instance by instance) | Slow (redeploy old version) | None | Low-risk changes, minimal infra |
| **Blue/Green** | Instant (DNS/LB flip) | Instant (flip back) | 2× capacity | Zero-downtime deploys; fast rollback critical |
| **Canary** | Gradual (% of traffic) | Fast (shift % back to 0) | Small extra capacity | Risky changes; validate on real traffic before full rollout |
| **Feature flags** | Per-user/cohort | Instant (toggle off) | None (logic in code) | Decouple deploy from release; A/B testing |

**Blue/green ops detail:** new version deployed to green (idle). Load balancer or Route 53 weighted routing switched to green. Blue stays live for ~15 min for instant rollback. After validation, blue is decommissioned or becomes the next green.

**Canary detail:** route 1% → 5% → 25% → 100% traffic, watching error rates and latency at each step. Automated promotion/rollback based on SLO burn rate is the mature implementation.

---

### Disaster Recovery

**RTO and RPO are the two numbers every DR design must answer:**

- **RTO (Recovery Time Objective):** maximum tolerable downtime — *"how long can we be down?"*
- **RPO (Recovery Point Objective):** maximum tolerable data loss — *"how much data can we lose?"*

**DR tiers (cost vs speed trade-off):**

| Tier | RTO | RPO | What's running | Relative cost |
|---|---|---|---|---|
| **Backup & restore** | Hours–days | Hours (last backup) | Nothing in DR region | Lowest |
| **Pilot light** | ~1 hour | Minutes | Core DB replication only; everything else off | Low |
| **Warm standby** | Minutes | Seconds | Scaled-down replica of full stack | Medium |
| **Active-active** | Near zero | Near zero | Full production in both regions | Highest |

**Operationalize it:** DR that is never tested is not DR — it is a hope. Test failover on a schedule (game days, chaos engineering). The only RTO number that matters is the one you measured during a real drill.

---

### CAP Theorem

During a **network partition**, a distributed system must choose:
- **Consistency (CP):** all nodes see the same data; some requests may fail or block
- **Availability (AP):** all requests receive a response; different nodes may return stale data

**No partition = no trade-off** — you can have both C and A when the network is healthy. The trade-off only applies during partition events.

| System | Choice | Example |
|---|---|---|
| Traditional RDBMS (single node) | CA (no partition tolerance) | PostgreSQL, MySQL |
| Distributed SQL | CP | Google Spanner, CockroachDB |
| Key-value stores (eventual consistency) | AP | DynamoDB (default), Cassandra, DynamoDB streams |
| Coordination services | CP | ZooKeeper, etcd |

**SRE framing:** don't memorise which quadrant; reason through it. *"For a payments service, consistency is non-negotiable — I'd accept brief unavailability over a double-charge. For a shopping cart, stale data is acceptable — I'd rather the user can add items than block on a partition."*

---

### Observability at Scale

**Three pillars (and when each matters):**

| Pillar | What it answers | Storage | Cardinality |
|---|---|---|---|
| **Metrics** | *Is the system healthy? Trend over time?* | Time-series DB (Prometheus, CloudWatch) | Low — pre-aggregated |
| **Logs** | *What exactly happened on this request?* | Object store / ELK | High — one entry per event |
| **Traces** | *Why was THIS request slow across microservices?* | Distributed trace store (Jaeger, X-Ray) | High — per-request spans |

**Metric ingestion at scale:** streaming aggregation (count/sum/histogram buckets computed at collection time) reduces storage by orders of magnitude vs storing raw events. Prometheus scrapes; StatsD/OpenTelemetry push. High cardinality labels (user IDs, request IDs in metrics) kill time-series databases — don't do it.

**SLO-based alerting (burn rate alerts):** alert when you're consuming the error budget too fast, not when a threshold is crossed. A 1% error rate at 3am is fine; a 1% error rate consuming 2× your daily budget in 1 hour is a page.

**Alert fatigue kills on-call:** every alert must be: actionable (there's something you can do), urgent (it can't wait until morning), and novel (not already being handled). Tune aggressively — a noisy alert is worse than no alert.

---

### System Design One-Liners

- "The circuit breaker turns a cascading failure into a fast, bounded failure — callers get 400ms errors instead of 30-second timeouts."
- "At-least-once delivery with idempotent consumers gives you exactly-once semantics without exactly-once overhead."
- "Read replicas solve read scale; sharding solves write scale. If you're not write-bound, don't shard."
- "Blue/green gives you a rollback in seconds; canary gives you blast-radius control. Use canary when you're not confident; blue/green when speed of rollback is critical."
- "RTO is how long you can be down; RPO is how much data you can lose. Design the system to meet both, then test it — untested DR is just a plan."
- "Consistent hashing means adding a cache node only moves k/n keys instead of rehashing everything."
- "The thundering herd hits the database the moment a popular cache key expires — solve it with probabilistic refresh or a mutex, not by making TTLs longer."
- "High cardinality in metrics labels (user IDs, trace IDs) will OOM your Prometheus in production — aggregate before ingesting."
- "CAP trade-off only matters during a partition — reason from the business cost of stale data vs unavailability, not the acronym."
