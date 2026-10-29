# Concurrent sandboxes: agentbox vs rootless Podman

**Question:** how much does each concurrent agentbox Sandbox cost when you run many of them on one machine? Does that cost stay flat from 10 to 500 sandboxes? How does it compare with rootless Podman, the standard daemonless container alternative?

**Verdict:**
- **Both systems ran 500 concurrent sandboxes** on a 15 GB laptop without failing.
- **agentbox is lighter and much faster at every scale tested.** At 500 sandboxes:

| At N = 500 | agentbox | Podman | |
|---|---:|---:|---|
| Memory, all 500 idle (`MemAvailable` drop) | 220 MB | 1.03 GB | 4.7x less |
| Memory per sandbox | 0.44 MB | 2.05 MB | |
| Start all 500 | 3.2 s | 42.3 s | 13x faster |
| Run one command in all 500 at once | 2.9 s | 32.5 s | 11x faster |
| CPU for that batch | 54 CPU-s | 435 CPU-s | 8x less |
| Tear all 500 down | 1.5 s | 56.4 s | 38x faster |

- **This depends on a fix made during this benchmark.** The first run found agentbox using about 2x *more* memory than Podman (4.6 MB per sandbox). Nearly all of that was fixed-size 256-checkpoint buffers, committed whether or not they were used. They were trimmed in `560874d`. See [Before the fix](#before-the-fix).

## Setup

**Machine and environment:**

| | |
|---|---|
| Machine | WSL2, kernel 6.18.54.1-microsoft-standard-WSL2, 22 CPUs, 15.3 GB RAM, ext4 |
| Power | Laptop on battery for both runs, so absolute times are likely slower than when plugged in |
| Power blip | Battery-saver mode came on for about 20 s during repeat 1 of the after-fix run. Every repeat-1 timing falls within the range of repeats 2–3, so the medians are unaffected. |
| Software | Python 3.14.7, Podman 3.4.4 (rootless, native overlay storage, crun) |
| Script | `PYTHONPATH=src python3 tests/benchmark_concurrency.py --out docs/benchmarks/concurrency-results.json` |
| Raw data | After the fix: [concurrency-results.json](concurrency-results.json). Before the fix (`4e99ce8`): [concurrency-results-before-fix.json](concurrency-results-before-fix.json). Each file holds every step of every repeat plus the summary. |

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
- **Failure rule:** a step fails on any error, if available memory drops below 1.5 GB, or if any phase takes more than 5 minutes. **No step failed in either run.**

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
| 10 | noise (−1.3 … 1.3) | noise (−7.6 … 10.3) | — | — |
| 50 | 0.1 (−0.1 … 0.5) | 4.4 | ~5 MB | 218 MB |
| 100 | 0.34 | 3.3 | 34 MB | 330 MB |
| 250 | 0.24 | 3.3 | 60 MB | 820 MB |
| 500 | 0.44 (0.39 … 0.47) | 2.05 (2.05 … 2.11) | 220 MB | 1.03 GB |

How to read this table:
- **Below about 100 sandboxes, the `MemAvailable` reading is mostly noise.** The ranges in brackets are wider than the values themselves.
- **The per-process figures are stable at every N,** so they give the dependable picture:

| | agentbox | Podman |
|---|---|---|
| Sandbox processes, PSS per sandbox | 0.20 MB | 0.37 MB |
| Driver memory growth per sandbox | ~0.008 MB | ~0 |
| `MemAvailable` drop per sandbox at N=500 | 0.44 MB | 2.05 MB |

What the second table shows:
- **Both systems have flat per-process cost,** so both scale linearly.
- **Most of each system's cost sits outside its own processes,** in kernel-side state: overlay mounts, namespaces and, for Podman, container storage and cgroup bookkeeping. Podman carries noticeably more of it.
- **The gap narrows after the load phase but remains.** After load, the total `MemAvailable` drop at N=500 was 100–220 MB for agentbox and about 510 MB for Podman (~1 MB per container).

**Idle CPU:** both are indistinguishable from zero. Background CPU on this WSL machine varied between 0.03 and 0.83 cores from step to step, which swamps any difference. Both designs block in `read()`/`pause()`/`poll()` while idle, so zero is expected.

### Startup and teardown

| N | agentbox: create all | Podman: create all | agentbox: create p95 | Podman: create p95 | agentbox: teardown | Podman: teardown |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 0.09 s | 1.35 s | 78 ms | 1,345 ms | 0.05 s | 1.82 s |
| 50 | 0.36 s | 4.58 s | 129 ms | 1,657 ms | 0.19 s | 6.09 s |
| 100 | 0.66 s | 8.45 s | 149 ms | 1,628 ms | 0.29 s | 11.26 s |
| 250 | 1.56 s | 20.02 s | 173 ms | 1,585 ms | 0.72 s | 27.40 s |
| 500 | 3.20 s | 42.34 s | 175 ms | 1,699 ms | 1.49 s | 56.39 s |

How it scales:
- **agentbox creation is now close to linear,** at 6–9 ms of wall time per sandbox. Its slowest creates (p95) level off at about 175 ms. Before the fix it took 10.8 s to create 500, with a p95 of 579 ms; committing ~4 MB per sandbox up front had been slowing startup too.
- **Podman is about 1.6 s per container, flat.** Most of that is the cost of starting the `podman` command itself.

### Under load (all N run the command at once)

| N | agentbox: batch wall time | Podman: batch wall time | agentbox: cmd p95 | Podman: cmd p95 | agentbox: CPU | Podman: CPU |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 0.07 s | 1.03 s | 63 ms | 1,017 ms | 0.6 CPU-s | 4.5 CPU-s |
| 50 | 0.39 s | 3.34 s | 317 ms | 3,084 ms | 5.7 CPU-s | 25.0 CPU-s |
| 100 | 0.70 s | 5.93 s | 477 ms | 5,375 ms | 11.9 CPU-s | 51.7 CPU-s |
| 250 | 1.45 s | 14.92 s | 926 ms | 13,694 ms | 26.3 CPU-s | 170.8 CPU-s |
| 500 | 2.89 s | 32.47 s | 2,175 ms | 29,687 ms | 54.2 CPU-s | 434.5 CPU-s |

What this shows:
- **Both scale roughly linearly in total work.** The rising p95 is 500 commands queuing for 22 cores.
- **agentbox spends about 0.11 CPU-s per command; Podman about 0.87 CPU-s.** Nearly all of Podman's gap is `podman exec` overhead (starting the CLI, then `conmon`, then `crun exec`), not the command itself.
- **An agent harness that keeps one long-lived shell open inside each container would close much of that gap.** That setup was not measured.

## Before the fix

The first run (`4e99ce8`, [raw data](concurrency-results-before-fix.json)) measured agentbox at **4.6 MB per idle sandbox at N=500, against Podman's 2.3 MB**. Podman was the lighter of the two then.

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

**Result:** supervisor + anchor PSS went from 3.3 MB to about 0.2 MB, and `sizeof(AgentboxBox)` from 1,112,088 to 12,300 bytes. `tests/test_sandbox_unit.py::test_idle_sandbox_memory_stays_small` guards against regressions.

**Side effect:** startup also got faster. Creating 500 sandboxes went from 10.8 s to 3.2 s, and the p95 create time from 579 ms to 175 ms. Load and teardown times did not change.

## Caveats

- **This is one machine, WSL2, on battery.** Absolute times will differ elsewhere; the ratios are the useful part.
- **Podman 3.4.4 is old.** Newer Podman releases are faster to start containers.
- **Small-N memory readings are noise.** Use the PSS and driver figures, or N ≥ 250.
- **Each agentbox sandbox was given an explicit `runtime_dir`.** Without one, sandboxes over the same workspace in one process collide ([#1](https://github.com/sramin24/deterministic-agentbox/issues/1)).
- **The single-sandbox README latency figures didn't reproduce on this machine** ([#2](https://github.com/sramin24/deterministic-agentbox/issues/2)). That is unrelated to the concurrency results.
