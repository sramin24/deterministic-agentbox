# Concurrent sandboxes: agentbox vs Podman and Docker

**Question:** how much does each concurrent agentbox Sandbox cost when you run many of them on one machine? Does that cost stay flat from 10 to 500 sandboxes? How does it compare with the standard container alternatives: rootless Podman, and Docker (Docker Desktop)?

**Verdict:**
- **All three systems ran 500 concurrent sandboxes** on a 15 GB laptop without failing.
- **agentbox is the lightest and the fastest at every scale tested.** At 500 sandboxes:

| At N = 500 | agentbox | Podman (rootless) | Docker (Desktop) |
|---|---:|---:|---:|
| Memory, all 500 idle (`MemAvailable` drop) | **205 MB** | 1.13 GB | 4.2 GB¹ |
| Process memory per sandbox (PSS) | **0.20 MB** | 0.37 MB | 3.86 MB |
| Shared engine (daemon) memory | **none** | none | ~4.7 GB² |
| Start all 500 | **2.1 s** | 41.2 s | 70.1 s |
| Run one command in all 500 at once | **1.5 s** | 33.4 s | 19.2 s |
| CPU for that batch | **29 CPU-s** | 471 CPU-s | 238 CPU-s |
| Tear all 500 down | **1.1 s** | 53.7 s | 62.3 s, then 45 s / 479 CPU-s of background cleanup |

¹ This includes about 1.5 GB of `dockerd` growth during the step.
² `dockerd` + `containerd`, resident after the warm-up round. It is not included in the row above. See [Docker](#docker).

- **agentbox's per-sandbox costs stay flat as N grows.** The CPU cost per command of both container systems does not: Podman's rises 3.7x and Docker's 2.9x between 10 and 500 containers. See [Where it starts to degrade](#where-it-starts-to-degrade).
- **This depends on a fix made during this benchmark.** The first run found agentbox using about 2x *more* memory than Podman (4.8 MB per sandbox). Nearly all of that was fixed-size 256-checkpoint buffers, committed whether or not they were used. They were trimmed in `560874d`. See [Before the fix](#before-the-fix).

## Setup

**Machine and environment:**

| | |
|---|---|
| Machine | WSL2, kernel 6.18.54.1-microsoft-standard-WSL2, 22 CPUs, 15.3 GB RAM, ext4 |
| Power | Plugged in, Windows power mode "Best power efficiency", the same for every run. "Best performance" would likely give faster absolute times; the ratios are what matter. |
| Software | Python 3.14.7. Podman 3.4.4 (rootless, native overlay storage, crun). Docker 28.5.1 (Docker Desktop, WSL2 backend, overlayfs). |
| Script | `PYTHONPATH=src python3 tests/benchmark_concurrency.py --out docs/benchmarks/concurrency-results.json` |
| agentbox and Podman, after the fix | [concurrency-results.json](concurrency-results.json) |
| agentbox and Podman, before the fix | [concurrency-results-before-fix.json](concurrency-results-before-fix.json): the same script, run against the engine at `4e99ce8` |
| Docker | [concurrency-results-docker.json](concurrency-results-docker.json): `--systems docker --warmup` |

Each JSON file holds every step of every repeat, the summary and the degrade points.

**Run order:**
- **agentbox and Podman** were measured in one run, alternating which went first in each repeat.
- **Docker** was measured in a separate run later the same day, under the same conditions.
- **Settle wait:** the Docker run also waited after each teardown until background CPU fell to 1 core or less (see [Docker](#docker)). The agentbox and Podman runs predate that wait, but their background CPU never exceeded 0.83 cores, so it would not have changed them.

**Earlier runs:** two earlier runs were made on battery (git history, `9eba9c9`). They showed the same memory figures and the same ranking. agentbox ran slower on battery (its command batches took about 2x longer than plugged in), while Podman's times were about the same. So the battery runs understated agentbox's lead.

**What one "sandbox" is:**
- **agentbox:** one `Sandbox(fixture, runtime_dir=<unique>)`. All sandboxes are layered over one shared ~1,000-file fixture, the same fixture as `benchmark_latency.py`.
- **Podman:**
  ```
  podman run -d --network host -v <fixture>:/ws:O alpine:3.20 sleep infinity
  ```
  `:O` gives each container its own copy-on-write overlay over the same shared fixture. That is the same isolation agentbox provides.
- **Docker:**
  ```
  docker run -d --network host <image> sleep infinity
  ```
  The image is `FROM alpine:3.20` + `COPY fixture /ws`. Docker has no equivalent of `:O`, so the fixture is baked into an image. Each container's own writable layer then gives the same copy-on-write view.
- **`--network host` for both container systems** matches agentbox, which shares the host network. It also skips per-container network setup, so these are the *lighter* container configurations.

**How each step ran:**
- **Scale steps:** 10, 50, 100, 250 and 500, with 3 repeats each. Tables show the median.
- **Creation and teardown:** run in parallel from one Python driver with 16 worker threads.
- **Idle measurement:** taken 2 s after the last sandbox is created.
- **Load measurement:** all N sandboxes run this command at the same time, 3 times per step:
  ```sh
  for i in $(seq 200); do echo $i > out_$i.txt; done
  find . -type f | xargs cat | md5sum
  ```
  agentbox runs it with `Sandbox.run()`; the container systems with `podman exec` / `docker exec`. The checksum must be identical in every sandbox, or the step fails.
- **Clean-state check:** after each teardown, the script checks that no `agentbox-exec` process or labelled container survived.
- **Failure rule:** a step fails on any error, if available memory drops below 1.5 GB, if a phase runs past 5 minutes, or if anything survives teardown. **No step failed in any run, and nothing survived teardown.**

**How memory is measured:**
- **Headline:** the drop in system-wide `MemAvailable` while the sandboxes are open. It counts everything: processes, kernel mounts and namespaces, the driver, and daemon growth.
  - **For Docker this still works.** Docker Desktop's engine runs in a separate WSL distro, but it shares the same kernel.
- **Breakdown:** the summed PSS (proportional memory) of each sandbox's own processes:
  - agentbox: `agentbox-exec` supervisor + anchor;
  - Podman: `conmon` + the container process;
  - Docker: `containerd-shim` + the container process, read through a `docker run --pid=host --cap-add SYS_PTRACE` helper container, because those processes are not visible from this distro's `/proc`.
- **Engine memory:** Docker's shared `dockerd` + `containerd` memory is reported separately.
- **Not used:** `podman stats`. On this machine Podman can't give each container its own cgroup, so `podman stats` reports a shared cgroup and gives wrong numbers.

## Results (median of 3, agentbox after the fix)

### Idle footprint

| N | agentbox MB/sandbox | Podman MB/sandbox | Docker MB/sandbox | agentbox total | Podman total | Docker total |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | noise (−3.6 … 0.5) | noise (−22.2 … 7.4) | noise (5.7 … 11.2) | — | — | — |
| 50 | 0.26 (0.11 … 0.74) | 3.53 (3.49 … 5.70) | 4.57 (3.53 … 7.20) | 13 MB | 177 MB | 229 MB |
| 100 | 0.13 (0.08 … 0.39) | 3.72 (2.57 … 5.06) | 2.33 (1.58 … 2.42) | 13 MB | 372 MB | 233 MB |
| 250 | 0.36 (0.23 … 0.48) | 3.10 (2.81 … 3.11) | noise (−8.9 … 5.9)³ | 90 MB | 775 MB | — |
| 500 | 0.41 (0.00 … 0.51) | 2.26 (2.22 … 2.28) | 8.44 (6.22 … 8.66) | 205 MB | 1.13 GB | 4.2 GB |

³ `dockerd` released about 1.9 GB of its own heap during this step, which swamps the containers' cost.

How to read this table:
- **At N = 10 the `MemAvailable` reading is noise for every system.** Even at 50–100, agentbox's few-MB totals sit close to the noise floor.
- **For Docker it is noisy at every N,** because `dockerd`'s heap swings by gigabytes.
- **The per-process figures are stable at every N,** so they give the dependable picture:

| | agentbox | Podman | Docker |
|---|---|---|---|
| Sandbox processes, PSS per sandbox | 0.20–0.23 MB | 0.56 → 0.37 MB | 4.83 → 3.86 MB |
| Driver memory growth per sandbox | ~0.009 MB | ~0 | ~0 |
| Shared engine | none | none | `dockerd` + `containerd`, 2.3–5.6 GB after warm-up (usually ~5 GB) |
| `MemAvailable` drop per sandbox at N=500 | 0.41 MB | 2.26 MB | 8.44 MB (incl. ~3 MB of `dockerd` growth) |

What the second table shows:
- **All three systems' per-process cost stays flat or falls,** so process memory scales linearly. Docker's per-container process (`containerd-shim`, a Go program) is about 19x agentbox's.
- **Most of agentbox's and Podman's cost sits outside their own processes,** in kernel-side state such as overlay mounts and namespaces. Podman carries considerably more of it.
- **The gap remains after the load phase.** At N=500 the `MemAvailable` drop was 106 MB for agentbox, 521 MB for Podman and 4.6 GB for Docker.

**Idle CPU:** agentbox and Podman are indistinguishable from zero. Background CPU varied between 0.02 and 0.76 cores from step to step in their run. Both designs block in `read()`/`pause()`/`poll()` while idle, so zero is expected. Docker was also near zero up to N=250 (at most 0.8 cores). At N=500 it kept about 14.6 cores busy in the 2–7 s after the last container was created; this benchmark did not measure whether that settles.

### Startup and teardown

| N | agentbox: create all (p50 / p95) | Podman: create all (p50 / p95) | Docker: create all (p50 / p95) |
|---:|---:|---:|---:|
| 10 | 0.06 s (35 / 49 ms) | 0.96 s (836 / 955 ms) | 0.57 s (541 / 561 ms) |
| 50 | 0.20 s (41 / 60 ms) | 3.31 s (903 / 1,215 ms) | 2.40 s (694 / 862 ms) |
| 100 | 0.39 s (46 / 76 ms) | 6.69 s (1,016 / 1,321 ms) | 5.20 s (765 / 1,094 ms) |
| 250 | 0.92 s (50 / 105 ms) | 17.47 s (1,047 / 1,498 ms) | 21.75 s (1,454 / 2,032 ms) |
| 500 | 2.05 s (57 / 107 ms) | 41.21 s (1,285 / 1,781 ms) | 70.10 s (2,144 / 3,548 ms) |

| N | agentbox: teardown | Podman: teardown | Docker: teardown | Docker: background cleanup after teardown |
|---:|---:|---:|---:|---:|
| 10 | 0.02 s | 1.26 s | 0.25 s | < 5 s, 0.4 CPU-s |
| 50 | 0.10 s | 4.77 s | 1.45 s | < 5 s, 2.4 CPU-s |
| 100 | 0.17 s | 10.27 s | 4.95 s | ~10 s, 16 CPU-s |
| 250 | 0.48 s | 24.84 s | 14.66 s | ~20 s, 167 CPU-s |
| 500 | 1.08 s | 53.67 s | 62.28 s | ~45 s, 479 CPU-s |

How it scales:
- **agentbox creation is close to linear,** at about 4 ms of wall time per sandbox with 16 workers. Its slowest creates (p95) level off at about 105 ms.
- **Podman is about 1 s per container,** and that grows slowly with N. Most of it is the cost of starting the `podman` command itself.
- **Docker starts faster than Podman at small N, but slows down as the container count grows.** Its p50 create time roughly quadruples from 10 to 500 containers (541 → 2,144 ms).
- **Docker's cleanup continues after `docker rm` returns.** The background work is measured in 5 s windows, so "< 5 s" means it was done within the first check.

### Under load (all N run the command at once)

| N | agentbox: batch (cmd p50 / p95) | Podman: batch (cmd p50 / p95) | Docker: batch (cmd p50 / p95) |
|---:|---:|---:|---:|
| 10 | 0.05 s (31 / 41 ms) | 0.89 s (600 / 880 ms) | 0.24 s (237 / 242 ms) |
| 50 | 0.25 s (106 / 210 ms) | 2.57 s (1,750 / 2,428 ms) | 1.03 s (950 / 968 ms) |
| 100 | 0.42 s (125 / 231 ms) | 4.92 s (3,332 / 4,570 ms) | 2.19 s (2,031 / 2,046 ms) |
| 250 | 0.84 s (232 / 452 ms) | 14.70 s (11,963 / 13,791 ms) | 7.29 s (6,788 / 6,840 ms) |
| 500 | 1.53 s (279 / 682 ms) | 33.44 s (27,707 / 31,474 ms) | 19.23 s (17,927 / 18,150 ms) |

| N | agentbox: CPU (per cmd) | Podman: CPU (per cmd) | Docker: CPU (per cmd) |
|---:|---:|---:|---:|
| 10 | 0.3 CPU-s (34 ms) | 2.6 CPU-s (257 ms) | 1.6 CPU-s (165 ms) |
| 50 | 3.0 CPU-s (60 ms) | 15.4 CPU-s (308 ms) | 9.0 CPU-s (180 ms) |
| 100 | 5.9 CPU-s (59 ms) | 41.2 CPU-s (412 ms) | 19.5 CPU-s (195 ms) |
| 250 | 15.1 CPU-s (60 ms) | 179.3 CPU-s (717 ms) | 75.0 CPU-s (300 ms) |
| 500 | 28.9 CPU-s (58 ms) | 470.8 CPU-s (942 ms) | 237.7 CPU-s (475 ms) |

What this shows:
- **agentbox spends a flat ~60 ms of CPU per command from N=50 upward.** Its rising command time is just 500 commands queuing for 22 cores.
- **Both container systems' CPU per command grows with the number of containers:** Podman's from 257 to 942 ms, and Docker's from 165 to 475 ms. Each `exec` gets more expensive the more containers exist. This benchmark doesn't isolate why.
- **Nearly all of the container systems' gap is `exec` overhead,** not the command itself. For Podman that is starting the CLI, then `conmon`, then `crun exec`; Docker's goes through the daemon.
- **An agent harness that keeps one long-lived shell open inside each container would avoid most of that overhead.** That setup was not measured.

### Where it starts to degrade

The script reports the first N at which a per-sandbox cost exceeds 2x its value at N=10:

| Metric | agentbox | Podman | Docker |
|---|---|---|---|
| Process memory per sandbox (PSS) | never (0.23 → 0.20 MB) | never (0.56 → 0.37 MB) | never (4.83 → 3.86 MB) |
| CPU per command | never (34 → 58 ms, at most 1.8x) | **N = 250** (257 → 717 ms; 942 ms at 500) | **N = 500** (165 → 475 ms) |
| Command p95 | N = 50 | N = 50 | N = 50 |

The command-p95 crossing happens at N = 50 for every system. It reflects contention for the 22 cores, not a cost per sandbox.

## Docker

Docker Desktop has costs the other two systems don't, because its engine is a long-running daemon:

- **Engine memory grows and is mostly kept.** `dockerd` + `containerd` used about 0.4 GB early in the session. After the warm-up round (one discarded 500-container step), they held between 2.3 and 5.6 GB, usually about 5 GB. They swung by up to ±2 GB between steps as Go's garbage collector released heap and then took it back. Restarting Docker Desktop returns that memory.
- **Without `--warmup`,** that growth lands inside the measured steps and makes the `MemAvailable` figures meaningless.
- **Teardown keeps going in the background.** After removing 500 containers, Docker kept an average of about 11 of the 22 cores busy for roughly 45 s, burning ~479 CPU-s. That is twice the CPU of the whole 500-command load batch.
- **The background cleanup contaminated a first Docker run.** Without a wait, it landed in whichever step came next: N=10 creates took 7 s instead of 0.6 s. The script now waits after each teardown until background CPU drops to 1 core or less. The wait and the CPU it burned are charged to the step that caused them.

## Before the fix

The same script, run against the engine at `4e99ce8` under the same conditions ([raw data](concurrency-results-before-fix.json)), measured agentbox at **4.76 MB per idle sandbox at N=500, against Podman's 2.18 MB**. Podman was the lighter of the two then.

**Where that memory went:**
- **Sandbox processes: 3.3 MB per sandbox (PSS).** 2.2 MB of the supervisor's was `[stack]`, and the anchor held another ~1 MB.
- **Driver: 1.05 MB per sandbox.** This is the Python-side `AgentboxBox`.

**The three causes, all fixed-size buffers sized for `AGENTBOX_MAX_CHECKPOINTS` (256):**

| Buffer | Size | Why it was committed even when unused |
|---|---:|---|
| `supervisor_state_t` on the supervisor's stack, `memset` to zero | ~1.1 MB | The `memset` writes every page. The anchor is created by `fork()` after Ubuntu's default `-fstack-clash-protection` has touched the frame, so it keeps its own ~1 MB copy. |
| `remount_current()`'s `char stack[4096 * 256]` | 1 MB | Stack-clash probing touches every page of a frame this large. |
| `agentbox_t.checkpoints[256]`, a client-side mirror of the checkpoint list | ~1.05 MB per Python `Sandbox` | ctypes zero-fills the struct. Nothing read the mirror, and it went wrong after `rollback(NULL)`. |

**The fix (`560874d`):**
- `supervisor_state_t` is now `calloc`'d, so its pages commit only as checkpoints are written.
- The lowerdir string is heap-allocated at its exact size.
- The client mirror is removed.

**Results at N=500, same conditions:**

| | Before the fix | After the fix |
|---|---:|---:|
| Memory per idle sandbox (`MemAvailable`) | 4.76 MB | 0.41 MB |
| Sandbox processes, PSS per sandbox | 3.32 MB | 0.20 MB |
| Driver growth, all 500 sandboxes | 525 MB | 4.3 MB |
| Memory after the load phase | 2.2 GB | 106 MB |
| Start all 500 (p95) | 7.7 s (462 ms) | 2.1 s (107 ms) |
| Command batch / teardown | 1.60 s / 1.12 s | 1.53 s / 1.08 s (unchanged) |

`sizeof(AgentboxBox)` fell from 1,112,088 to 12,300 bytes. `tests/test_sandbox_unit.py::test_idle_sandbox_memory_stays_small` guards against regressions.

**API change:** `agentbox_t` no longer has `checkpoints` or `checkpoint_count`. The Python package was the only code using them, and it is updated.

## Caveats

- **This is one machine, WSL2, plugged in on "Best power efficiency".** Absolute times will differ elsewhere; the ratios are the useful part.
- **Podman 3.4.4 is old.** Newer Podman releases are faster to start and exec containers, and may scale better with container count.
- **Docker was measured through Docker Desktop's WSL2 backend.** Docker Engine running natively on Linux has the same daemon architecture but no Desktop layer, so its numbers may differ.
- **Docker was measured in a separate run from agentbox and Podman,** the same day and under the same power setting.
- **Small-N memory readings are noise, and Docker's are noisy at every N.** Use the PSS figures, or N = 500.
- **Each agentbox sandbox was given an explicit `runtime_dir`.** Without one, sandboxes over the same workspace in one process collide ([#1](https://github.com/sramin24/deterministic-agentbox/issues/1)).
- **The 5-minute phase limit is only partly enforced.** The script stops waiting for results after 5 minutes, but tasks already running still finish before it moves on, and teardown has no limit. No phase came near 5 minutes.
- **The single-sandbox README latency figures didn't reproduce on this machine** ([#2](https://github.com/sramin24/deterministic-agentbox/issues/2)). That is unrelated to the concurrency results.
