# Concurrent sandboxes: agentbox vs rootless Podman

**Question:** how much does each concurrent agentbox Sandbox cost when you run many of them on one machine? Does that cost stay flat from 10 to 500 sandboxes? How does it compare with rootless Podman, the standard daemonless container alternative?

**Verdict:**
- **Both systems ran 500 concurrent sandboxes** on a 15 GB laptop without failing.
- **agentbox is lighter and much faster at every scale tested.** At 500 sandboxes:

| At N = 500 | agentbox | Podman | |
|---|---:|---:|---|
| Memory, all 500 idle (`MemAvailable` drop) | 205 MB | 1.13 GB | 5.5x less |
| Memory per sandbox | 0.41 MB | 2.26 MB | |
| Start all 500 | 2.1 s | 41.2 s | 20x faster |
| Run one command in all 500 at once | 1.5 s | 33.4 s | 22x faster |
| CPU for that batch | 29 CPU-s | 471 CPU-s | 16x less |
| Tear all 500 down | 1.1 s | 53.7 s | 50x faster |

- **agentbox's per-sandbox costs stay flat as N grows.** Podman's CPU cost per command does not: it rises 3.7x between 10 and 500 containers (see [Where it starts to degrade](#where-it-starts-to-degrade)).
- **This depends on a fix made during this benchmark.** The first run found agentbox using about 2x *more* memory than Podman (4.8 MB per sandbox). Nearly all of that was fixed-size 256-checkpoint buffers, committed whether or not they were used. They were trimmed in `560874d`. See [Before the fix](#before-the-fix).

## Setup

**Machine and environment:**

| | |
|---|---|
| Machine | WSL2, kernel 6.18.54.1-microsoft-standard-WSL2, 22 CPUs, 15.3 GB RAM, ext4 |
| Power | Plugged in, Windows power mode "Best power efficiency", the same for both runs. "Best performance" would likely give faster absolute times; the ratios are what matter. |
| Software | Python 3.14.7, Podman 3.4.4 (rootless, native overlay storage, crun) |
| Script | `PYTHONPATH=src python3 tests/benchmark_concurrency.py --out docs/benchmarks/concurrency-results.json` |
| After the fix | [concurrency-results.json](concurrency-results.json) |
| Before the fix | [concurrency-results-before-fix.json](concurrency-results-before-fix.json): the same script, run against the engine at `4e99ce8` |

Each JSON file holds every step of every repeat, the summary and the degrade points.

**Earlier runs:** two earlier runs were made on battery (git history, `9eba9c9`). They showed the same memory figures and the same ranking. agentbox ran slower on battery (its command batches took about 2x longer than plugged in), while Podman's times were about the same. So the battery runs understated agentbox's lead.

**What one "sandbox" is:**
- **agentbox:** one `Sandbox(fixture, runtime_dir=<unique>)`. All sandboxes are layered over one shared ~1,000-file fixture, the same fixture as `benchmark_latency.py`.
- **Podman:**
  ```
  podman run -d --network host -v <fixture>:/ws:O alpine:3.20 sleep infinity
  ```
  - `:O` gives each container its own copy-on-write overlay over the same shared fixture. That is the same isolation agentbox provides.
  - `--network host` matches agentbox, which shares the host network. It also spares Podman a `slirp4netns` process per container, so this is the *lighter* Podman setup.

**How each step ran:**
- **Scale steps:** 10, 50, 100, 250 and 500, with 3 repeats each. agentbox and Podman alternate which goes first in each repeat. Tables show the median.
- **Creation and teardown:** run in parallel from one Python driver with 16 worker threads.
- **Idle measurement:** taken 2 s after the last sandbox is created.
- **Load measurement:** all N sandboxes run this command at the same time, 3 times per step:
  ```sh
  for i in $(seq 200); do echo $i > out_$i.txt; done
  find . -type f | xargs cat | md5sum
  ```
  agentbox runs it with `Sandbox.run()`; Podman with `podman exec`. The checksum must be identical in every sandbox, or the step fails.
- **Clean-state check:** after each teardown, the script checks that no `agentbox-exec` process or labelled container survived.
- **Failure rule:** a step fails on any error, if available memory drops below 1.5 GB, if a phase runs past 5 minutes, or if anything survives teardown. **No step failed in either run, and nothing survived teardown.**

**How memory is measured:**
- **Headline:** the drop in system-wide `MemAvailable` while the sandboxes are open. It counts everything: processes, kernel mounts and namespaces, and the driver.
- **Breakdown:** the summed PSS (proportional memory) of each sandbox's own processes:
  - agentbox: `agentbox-exec` supervisor + anchor;
  - Podman: `conmon` + the container process.
- **Not used:** `podman stats`. On this machine Podman can't give each container its own cgroup, so `podman stats` reports a shared cgroup and gives wrong numbers.

**Not compared: Docker.** Docker Desktop on WSL runs its containers in a separate VM, whose memory isn't visible from this environment.

## Results (median of 3, after the fix)

### Idle footprint

| N | agentbox MB/sandbox | Podman MB/sandbox | agentbox total | Podman total |
|---:|---:|---:|---:|---:|
| 10 | noise (−3.6 … 0.5) | noise (−22.2 … 7.4) | — | — |
| 50 | 0.26 (0.11 … 0.74) | 3.53 (3.49 … 5.70) | 13 MB | 177 MB |
| 100 | 0.13 (0.08 … 0.39) | 3.72 (2.57 … 5.06) | 13 MB | 372 MB |
| 250 | 0.36 (0.23 … 0.48) | 3.10 (2.81 … 3.11) | 90 MB | 775 MB |
| 500 | 0.41 (0.00 … 0.51) | 2.26 (2.22 … 2.28) | 205 MB | 1.13 GB |

How to read this table:
- **At N = 10 the `MemAvailable` reading is pure noise.** Even at 50–100, agentbox's few-MB totals sit close to the noise floor.
- **The per-process figures are stable at every N,** so they give the dependable picture:

| | agentbox | Podman |
|---|---|---|
| Sandbox processes, PSS per sandbox | 0.20–0.23 MB | 0.56 MB at N=10, falling to 0.37 MB at N=500 |
| Driver memory growth per sandbox | ~0.009 MB | ~0 |
| `MemAvailable` drop per sandbox at N=500 | 0.41 MB | 2.26 MB |

What the second table shows:
- **Both systems' per-process cost stays flat or falls,** so memory scales linearly. Podman's falls slightly as shared pages spread over more containers.
- **Most of each system's cost sits outside its own processes,** in kernel-side state: overlay mounts, namespaces and, for Podman, container storage and cgroup bookkeeping. Podman carries considerably more of it.
- **The gap remains after the load phase.** At N=500 the `MemAvailable` drop was 106 MB for agentbox and 521 MB for Podman.

**Idle CPU:** both are indistinguishable from zero. Background CPU on this WSL machine varied between 0.02 and 0.76 cores from step to step, which swamps any difference. Both designs block in `read()`/`pause()`/`poll()` while idle, so zero is expected.

### Startup and teardown

| N | agentbox: create all | Podman: create all | agentbox create p50 / p95 | Podman create p50 / p95 | agentbox: teardown | Podman: teardown |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 0.06 s | 0.96 s | 35 / 49 ms | 836 / 955 ms | 0.02 s | 1.26 s |
| 50 | 0.20 s | 3.31 s | 41 / 60 ms | 903 / 1,215 ms | 0.10 s | 4.77 s |
| 100 | 0.39 s | 6.69 s | 46 / 76 ms | 1,016 / 1,321 ms | 0.17 s | 10.27 s |
| 250 | 0.92 s | 17.47 s | 50 / 105 ms | 1,047 / 1,498 ms | 0.48 s | 24.84 s |
| 500 | 2.05 s | 41.21 s | 57 / 107 ms | 1,285 / 1,781 ms | 1.08 s | 53.67 s |

How it scales:
- **agentbox creation is close to linear,** at about 4 ms of wall time per sandbox with 16 workers. Its slowest creates (p95) level off at about 105 ms.
- **Podman is about 1 s per container,** and that grows slowly with N. Most of it is the cost of starting the `podman` command itself.

### Under load (all N run the command at once)

| N | agentbox: batch wall time | Podman: batch wall time | agentbox cmd p50 / p95 | Podman cmd p50 / p95 | agentbox CPU (per cmd) | Podman CPU (per cmd) |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 0.05 s | 0.89 s | 31 / 41 ms | 600 / 880 ms | 0.3 CPU-s (34 ms) | 2.6 CPU-s (257 ms) |
| 50 | 0.25 s | 2.57 s | 106 / 210 ms | 1,750 / 2,428 ms | 3.0 CPU-s (60 ms) | 15.4 CPU-s (308 ms) |
| 100 | 0.42 s | 4.92 s | 125 / 231 ms | 3,332 / 4,570 ms | 5.9 CPU-s (59 ms) | 41.2 CPU-s (412 ms) |
| 250 | 0.84 s | 14.70 s | 232 / 452 ms | 11,963 / 13,791 ms | 15.1 CPU-s (60 ms) | 179.3 CPU-s (717 ms) |
| 500 | 1.53 s | 33.44 s | 279 / 682 ms | 27,707 / 31,474 ms | 28.9 CPU-s (58 ms) | 470.8 CPU-s (942 ms) |

What this shows:
- **agentbox spends a flat ~60 ms of CPU per command from N=50 upward.** Its rising command time is just 500 commands queuing for 22 cores.
- **Podman's CPU per command rises from 257 ms to 942 ms as the container count grows.** Each `podman exec` gets more expensive the more containers exist. That is consistent with Podman reading the state of every container on each call, though this benchmark doesn't isolate the cause.
- **Nearly all of Podman's gap is `podman exec` overhead** (starting the CLI, then `conmon`, then `crun exec`), not the command itself.
- **An agent harness that keeps one long-lived shell open inside each container would avoid most of that overhead.** That setup was not measured.

### Where it starts to degrade

The script reports the first N at which a per-sandbox cost exceeds 2x its value at N=10:

| Metric | agentbox | Podman |
|---|---|---|
| Process memory per sandbox (PSS) | never (0.23 → 0.20 MB) | never (0.56 → 0.37 MB) |
| CPU per command | never (34 → 58 ms, at most 1.8x) | **N = 250** (257 → 717 ms; 942 ms at 500) |
| Command p95 | N = 50 | N = 50 |

The command-p95 crossing happens at N = 50 for both systems. It reflects contention for the 22 cores, not a cost per sandbox.

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
- **Small-N memory readings are noise.** Use the PSS and driver figures, or N ≥ 250.
- **Each agentbox sandbox was given an explicit `runtime_dir`.** Without one, sandboxes over the same workspace in one process collide ([#1](https://github.com/sramin24/deterministic-agentbox/issues/1)).
- **The 5-minute phase limit is only partly enforced.** The script stops waiting for results after 5 minutes, but tasks already running still finish before it moves on, and teardown has no limit. No phase came near 5 minutes in these runs.
- **The single-sandbox README latency figures didn't reproduce on this machine** ([#2](https://github.com/sramin24/deterministic-agentbox/issues/2)). That is unrelated to the concurrency results.
