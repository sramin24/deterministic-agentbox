# Concurrent sandboxes: agentbox vs rootless Podman

**Question:** how much does each concurrent agentbox Sandbox cost when you run many of them on one machine? Does that cost stay flat from 10 to 500 sandboxes? How does it compare with rootless Podman, the standard daemonless container alternative?

**Verdict:**
- **Both systems ran 500 concurrent sandboxes** on a 15 GB laptop without failing. Neither came close to running out of memory.
- **agentbox is much faster.** At 500 sandboxes:
  - it starts them 4x faster;
  - it runs a command batch across all of them 11x faster, using 7.7x less CPU;
  - it tears them down 37x faster.
- **agentbox currently uses about 2x more memory per idle sandbox** (about 4.5 MB against Podman's 2.3 MB). Nearly all of that is fixed-size buffers sized for 256 checkpoints (see [Where agentbox's memory goes](#where-agentboxs-memory-goes)). It is not an inherent cost of the design, and it is fixable.
- **"Run 100 agents on a laptop that would choke Docker" is not supported by this data.** "Lower latency and less CPU per agent command" is supported.

## Setup

**Machine and environment:**

| | |
|---|---|
| Machine | WSL2, kernel 6.18.54.1-microsoft-standard-WSL2, 22 CPUs, 15.3 GB RAM, ext4 |
| Power | Laptop on battery for the whole run, so absolute times are likely slower than when plugged in |
| Software | Python 3.14.7, Podman 3.4.4 (rootless, native overlay storage, crun), `make build` at `bb74b55` |
| Script | `PYTHONPATH=src python3 tests/benchmark_concurrency.py --out docs/benchmarks/concurrency-results.json` |
| Raw data | [concurrency-results.json](concurrency-results.json), with every step of every repeat plus the summary |

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
- **Failure rule:** a step fails on any error, if available memory drops below 1.5 GB, or if any phase takes more than 5 minutes. **No step failed.**

**How memory is measured:**
- **Headline:** the drop in system-wide `MemAvailable` while the sandboxes are open. It counts everything: processes, kernel mounts and namespaces, and the driver.
- **Breakdown:** the summed PSS (proportional memory) of each sandbox's own processes:
  - agentbox: `agentbox-exec` supervisor + anchor;
  - Podman: `conmon` + the container process.
- **Not used:** `podman stats`. On this machine Podman can't give each container its own cgroup, so `podman stats` reports a shared cgroup and gives wrong numbers.

**Not compared: Docker.** Docker Desktop on WSL runs its containers in a separate VM, whose memory isn't visible from this environment.

## Results (median of 3)

### Idle footprint

| N | agentbox MB/sandbox | Podman MB/sandbox | agentbox total | Podman total |
|---:|---:|---:|---:|---:|
| 10 | ~0 (noise: −2.2 … 7.9) | 9.5 (noise: −11.8 … 11.7) | — | — |
| 50 | 1.4 | 7.3 | 68 MB | 365 MB |
| 100 | 2.8 | 4.9 | 282 MB | 491 MB |
| 250 | 4.3 | 3.2 | 1.08 GB | 0.81 GB |
| 500 | 4.6 | 2.3 | 2.29 GB | 1.14 GB |

How to read this table:
- **Below about 100 sandboxes, the `MemAvailable` reading is mostly noise.** The ranges in brackets are wider than the values themselves.
- **The per-process figures are stable at every N,** so they give the dependable picture:

| | agentbox | Podman |
|---|---|---|
| Sandbox processes, PSS per sandbox | 3.3 MB | 0.4 MB |
| Driver memory growth per sandbox | 1.05 MB | ~0 |
| **Measured processes, per sandbox** | **~4.4 MB** | **~0.4 MB** |
| `MemAvailable` drop per sandbox at N=500 | 4.6 MB | 2.3 MB |

What the second table shows:
- **agentbox's cost is almost entirely in its own processes,** so it scales linearly at about 4.4 MB per sandbox.
- **Podman's cost is mostly outside its own processes.** It sits in kernel-side and storage state (overlay mounts, namespaces, container storage). The per-sandbox figure falls as N grows because Podman's fixed costs are shared across more containers.
- **Podman's idle figure may overstate its steady-state cost.** After the load phase, its total `MemAvailable` drop at N=500 was only 0.5 GB (about 1 MB per container), against 1.1 GB when idle. agentbox's stayed at about 2.3 GB.

**Idle CPU:** both are indistinguishable from zero. Background CPU on this WSL machine varied between 0.06 and 0.83 cores from step to step, which swamps any difference. Both designs block in `read()`/`pause()`/`poll()` while idle, so zero is expected.

### Startup and teardown

| N | agentbox: create all | Podman: create all | agentbox: create p95 | Podman: create p95 | agentbox: teardown | Podman: teardown |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 0.13 s | 1.36 s | 95 ms | 1,335 ms | 0.05 s | 1.67 s |
| 50 | 0.67 s | 4.34 s | 151 ms | 1,589 ms | 0.19 s | 5.83 s |
| 100 | 1.24 s | 8.82 s | 268 ms | 1,727 ms | 0.34 s | 10.99 s |
| 250 | 4.15 s | 19.50 s | 448 ms | 1,549 ms | 0.68 s | 26.66 s |
| 500 | 10.84 s | 42.18 s | 579 ms | 1,690 ms | 1.46 s | 54.74 s |

How it scales:
- **agentbox creation time grows a little faster than linear.** Per sandbox it goes from 13 ms at N=10 to 22 ms at N=500. Its slowest creates (p95) go from 95 to 579 ms as the 16 workers contend.
- **Podman is about 1.5 s per container, flat.** Most of that is the cost of starting the `podman` command itself.

### Under load (all N run the command at once)

| N | agentbox: batch wall time | Podman: batch wall time | agentbox: cmd p95 | Podman: cmd p95 | agentbox: CPU | Podman: CPU |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 0.07 s | 1.08 s | 61 ms | 1,071 ms | 0.7 CPU-s | 4.3 CPU-s |
| 50 | 0.37 s | 3.07 s | 279 ms | 2,930 ms | 5.2 CPU-s | 22.9 CPU-s |
| 100 | 0.70 s | 5.98 s | 493 ms | 5,420 ms | 11.4 CPU-s | 50.9 CPU-s |
| 250 | 1.42 s | 14.22 s | 919 ms | 13,188 ms | 25.7 CPU-s | 165.6 CPU-s |
| 500 | 2.86 s | 31.32 s | 1,861 ms | 28,905 ms | 54.8 CPU-s | 420.7 CPU-s |

What this shows:
- **Both scale roughly linearly in total work.** The rising p95 is 500 commands queuing for 22 cores.
- **agentbox spends about 0.11 CPU-s per command; Podman about 0.84 CPU-s.** Nearly all of Podman's gap is `podman exec` overhead (starting the CLI, then `conmon`, then `crun exec`), not the command itself.
- **An agent harness that keeps one long-lived shell open inside each container would close much of that gap.** That setup was not measured.

## Where agentbox's memory goes

The breakdown of one idle supervisor's memory (`/proc/<pid>/smaps`, by mapping):

| Mapping | PSS |
|---|---:|
| `[stack]` | 2,162 kB |
| libc | 46 kB |
| `agentbox-exec` binary | 32 kB |
| anon + heap | 28 kB |

**Two fixed-size stack buffers in `src/c/agentbox_core.c` account for the stack:**
- **`supervisor_state_t st`** in `agentbox_supervisor_main()`, about 1.1 MB. Its `checkpoints[AGENTBOX_MAX_CHECKPOINTS]` array is 256 × ~4.3 KB, and it is `memset` to zero.
- **`char stack[AGENTBOX_PATH_LEN * AGENTBOX_MAX_CHECKPOINTS]`** in `remount_current()`, 1 MB.

**Why mostly empty buffers still use memory:** Ubuntu's gcc enables `-fstack-clash-protection` by default, which touches every page of a large stack frame. So both buffers are fully committed even though they are almost entirely empty.

**The anchor process holds a second copy.** It is created by `fork()` after the frame has been touched, so it keeps its own ~1 MB copy. That brings the total to 3.3 MB per sandbox.

**The client side has the same fixed table.** `agentbox_t`, mirrored as `AgentboxBox` in `_core.py`, embeds the same 256-entry checkpoint table, so each Python `Sandbox` holds 1.05 MB in the driver.

**Not yet verified:** sizing these by actual use (a growable checkpoint list, heap-allocated buffers, and forking the anchor before the big frame exists) should bring agentbox to roughly 0.2–0.3 MB of process memory per sandbox. That is below Podman's. This needs a fix and a re-run to confirm.

## Caveats

- **This is one machine, WSL2, on battery.** Absolute times will differ elsewhere; the ratios are the useful part.
- **Podman 3.4.4 is old.** Newer Podman releases are faster to start containers.
- **Small-N memory readings are noise.** Use the PSS and driver figures, or N ≥ 250.
- **Each agentbox sandbox was given an explicit `runtime_dir`.** Without one, sandboxes over the same workspace in one process collide ([#1](https://github.com/sramin24/deterministic-agentbox/issues/1)).
- **The single-sandbox README latency figures didn't reproduce on this machine** ([#2](https://github.com/sramin24/deterministic-agentbox/issues/2)). That is unrelated to the concurrency results.
