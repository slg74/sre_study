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

---
---

# Part 3 — Containers & Kubernetes

## 16. Containers Under the Hood (the "what IS a container" faker question)

**The answer:** a container is **not a VM** — it's just a Linux process (tree) wrapped in three kernel features:
1. **Namespaces** = what the process can *see* (pid, net, mnt, uts, ipc, user — its own PID 1, its own network stack, its own filesystem view)
2. **cgroups** = what the process can *use* (CPU, memory, pids limits — the exit-137 machinery from Part 1)
3. **Layered filesystem** (overlayfs) = image layers stacked read-only + a writable layer on top

Say it like: *"A container is a process with namespaces for isolation, cgroups for limits, and an overlay filesystem — same kernel as the host, which is why `ps` on the host shows every container's processes."*

**Direct Part 1 tie-ins (interviewers chain these deliberately):**
- Container memory limit = cgroup limit → exceed it → **OOMKilled, exit 137** — host free RAM irrelevant.
- **Your app is PID 1 inside the container** → it inherits init's reaping duty → apps that fork but don't reap fill the container with zombies. Fix: `docker run --init` / ECS `initProcessEnabled` (injects **tini**, a tiny init that just reaps).
- Stop sequence: SIGTERM → grace period → SIGKILL. PID 1 also has weird signal defaults (no default SIGTERM handler!) — another reason tini exists.
- Image vs container: image = read-only template/layers; container = running process + writable layer.

## 17. Kubernetes Core

**Architecture in one breath:** Control plane = **API server** (front door, everything goes through it), **etcd** (the database — lose it, lose the cluster), **scheduler** (picks nodes), **controller-manager** (reconciliation loops). Each node runs **kubelet** (starts/monitors pods), a container runtime (containerd), and **kube-proxy** (programs Service routing via iptables/IPVS).

**The mental model that marks a senior:** Kubernetes is a **reconciliation engine** — you declare desired state in etcd, controllers loop forever making reality match. (Same philosophy as Terraform, but continuous.)

- **Pod** = smallest unit; one or more containers sharing a network namespace (localhost to each other) and volumes. Pods are cattle — they get replaced, not fixed.
- **Deployment → ReplicaSet → Pods**: Deployment manages rollouts; each new version = new ReplicaSet. `kubectl rollout undo` = instant rollback. Tune with maxSurge/maxUnavailable.
- **Service** = stable virtual IP + DNS over an ever-changing pod set, selected by labels. Types: ClusterIP (internal), NodePort, LoadBalancer (cloud LB). **Ingress** for L7 routing. DNS: `svc-name.namespace.svc.cluster.local` via CoreDNS.
- **Probes — know the difference cold (classic filter question):**
  - **Liveness** fails → kubelet RESTARTS the container. 
  - **Readiness** fails → pod is REMOVED from Service endpoints (no restart).
  - **Startup** → holds off the other probes for slow-booting apps.
  - The classic self-inflicted outage: aggressive liveness probe + slow dependency = cluster-wide restart storm of healthy-but-slow pods. Liveness should check "am I deadlocked," never "is my database up."
- **Requests vs limits (the other guaranteed question):**
  - **Requests** = what the scheduler reserves (placement math).
  - **Limits** = what the cgroup enforces: CPU over limit → **throttled** (slow); memory over limit → **OOMKilled** (137, dead). 
  - QoS classes: Guaranteed (req=limit) > Burstable > BestEffort — eviction order under node memory pressure, worst class first.
- **HPA** scales replicas on metrics; **PDB** (PodDisruptionBudget) limits voluntary disruptions during node drains.
- **RBAC** in one line: Role/ClusterRole = verbs on resources; RoleBinding attaches them to users/ServiceAccounts.
- **EKS specifics** (likely their flavor): managed control plane, node groups or Fargate profiles, and **IRSA** — IAM Roles for Service Accounts = the K8s analog of ECS task roles: per-pod AWS permissions, no node-wide creds. (Ties to §11.)

## 18. K8s Debugging Playbook (scenario-question ammo)

**Universal first moves:** `kubectl describe pod X` (read Events — the answer is usually printed there), `kubectl logs X --previous` (the crashed container's logs, not the new one's), `kubectl get events --sort-by=.lastTimestamp`.

| Symptom | Usual story | Where to look |
|---|---|---|
| **CrashLoopBackOff** | App starts and dies; backoff doubles between restarts | `logs --previous`; exit code in describe: 1 = app error, **137 = OOM or kill**, 143 = SIGTERM |
| **OOMKilled** | Memory limit too low or app leak | describe shows OOMKilled + 137; bump limit or fix app; `kubectl top pod` for usage |
| **ImagePullBackOff** | Bad tag, missing registry auth (imagePullSecrets/IRSA), or no NAT path to registry | describe Events spells it out |
| **Pending forever** | Scheduler can't place it: insufficient requests-capacity, taints w/o tolerations, affinity rules, or unbound PVC | describe Events: "0/N nodes available: ..." tells you exactly why |
| **Readiness failing / no traffic** | Wrong port, app slow to warm, dependency check in readiness probe | describe + endpoint list: `kubectl get endpoints svc-name` |
| **Node NotReady** | kubelet down, disk/memory pressure, network partition | `kubectl describe node`: Conditions + taints; then SSH and it's Part 1 territory (D-states, OOM, full disk) |
| **Evictions** | Node memory/disk pressure → BestEffort/Burstable pods killed | `kubectl get events`; fix requests/limits honesty |

**The closing line for any K8s debugging answer:** "...and if the node itself is sick, I'm back to Linux fundamentals — kubelet logs, dmesg, D-states, disk pressure. Kubernetes is a scheduler on top of everything in Part 1."

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
