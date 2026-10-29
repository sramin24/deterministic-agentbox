"""Benchmark: many concurrent agentbox Sandboxes on one machine, against the
same number of rootless Podman (or Docker) containers.

For each scale step (default 10, 50, 100, 250, 500) and each system, it
creates N sandboxes over ONE shared fixture workspace, measures what they
cost while idle, runs a short command in all N at once, then tears them
down. A Podman "sandbox" is the closest equivalent to an agentbox one:

    podman run -d --network host -v <fixture>:/ws:O alpine:3.20 sleep infinity

`:O` gives each container its own copy-on-write overlay over the shared
fixture, which is exactly what agentbox gives each Sandbox. `--network host`
matches agentbox (which shares the host network) and spares Podman a
slirp4netns process per container -- i.e. it is the lighter Podman setup.

Docker (`--systems docker`) has no `:O` overlay volume, so the fixture is
baked into an image and each container's own writable layer gives the same
copy-on-write view. Measured against Docker Desktop's WSL2 backend: dockerd
runs in another WSL distro on the same kernel, so MemAvailable sees its
containers, while their processes are read through a `--pid=host` helper
container. dockerd + containerd are reported separately as engine memory.

Memory is measured two ways, the same way for every system:
  - system-wide MemAvailable drop (/proc/meminfo) -- the headline number,
    since it includes kernel-side cost (mounts, namespaces, dentries);
  - summed PSS (/proc/<pid>/smaps_rollup) of each sandbox's own processes:
    agentbox-exec supervisor + anchor, conmon + the container process
    (Podman), or containerd-shim + the container process (Docker).
`podman stats` is deliberately not used: on rootless setups without cgroup
delegation (e.g. WSL) it reports a shared cgroup, not the container.

A system stops ramping at the first failing step: any create/run error,
MemAvailable under --min-free-mb, or a single phase over --phase-timeout.

Run from the repo root after `make build`:
    PYTHONPATH=src python3 tests/benchmark_concurrency.py
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

from agentbox import Sandbox
from benchmark_latency import FILE_COUNT, build_fixture

PODMAN_IMAGE = "docker.io/library/alpine:3.20"
DOCKER_IMAGE = "agentbox-bench-fixture:latest"
BENCH_LABEL = "agentbox-bench=1"  # only containers with this label are ever removed
LOAD_CMD = (
    "for i in $(seq 200); do echo $i > out_$i.txt; done; "
    "find . -type f | xargs cat | md5sum"
)
SETTLE_SEC = 2.0
IDLE_CPU_WINDOW_SEC = 5.0


class StepFailed(Exception):
    def __init__(self, message: str, partial: list | None = None):
        super().__init__(message)
        self.partial = partial or []  # whatever completed before the failure


# ---- measurement helpers --------------------------------------------------

def mem_available_mb() -> float:
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) / 1024
    raise RuntimeError("MemAvailable missing from /proc/meminfo")


def cpu_busy_jiffies() -> int:
    with open("/proc/stat") as f:
        fields = [int(x) for x in f.readline().split()[1:]]
    idle = fields[3] + fields[4]  # idle + iowait
    return sum(fields[:8]) - idle


def jiffies_to_sec(j: int) -> float:
    return j / os.sysconf("SC_CLK_TCK")


def pss_kb(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            for line in f:
                if line.startswith("Pss:"):
                    return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        pass
    return 0


def descendants(pid: int) -> list[int]:
    """pid plus every process below it, via /proc/<pid>/task/*/children."""
    out, stack = [], [pid]
    while stack:
        p = stack.pop()
        out.append(p)
        try:
            for tid in os.listdir(f"/proc/{p}/task"):
                with open(f"/proc/{p}/task/{tid}/children") as f:
                    stack.extend(int(c) for c in f.read().split())
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return out


def tree_pss_kb(root_pids: list[int]) -> tuple[int, int]:
    """(summed PSS in kB, process count) over the trees rooted at root_pids."""
    pids = {p for r in root_pids for p in descendants(r)}
    return sum(pss_kb(p) for p in pids), len(pids)


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    k = min(len(ordered) - 1, max(0, round(pct / 100 * (len(ordered) - 1))))
    return ordered[k]


# ---- the systems, behind one tiny interface ------------------------------
# prepare(); create(i) -> handle; process_pss(handles) -> (kB, process count);
# engine_pss_mb() -> shared daemon memory, or None if there is no daemon;
# run(handle) -> (exit_code, stdout); destroy(handle)

class AgentboxSystem:
    name = "agentbox"

    def __init__(self, fixture: str, scratch: str):
        self.fixture = fixture
        self.scratch = scratch

    def create(self, i: int) -> Sandbox:
        # Explicit runtime_dir per Sandbox: the default name is derived from
        # the caller's pid, so many Sandboxes over one workspace from one
        # process would otherwise share (and clobber) one runtime_dir.
        return Sandbox(self.fixture, runtime_dir=os.path.join(self.scratch, f"rt_{i}"))

    def prepare(self) -> None:
        pass

    def process_pss(self, sandboxes: list[Sandbox]) -> tuple[int, int]:
        return tree_pss_kb([sb._box.supervisor_pid for sb in sandboxes])

    def engine_pss_mb(self) -> float | None:
        return None

    def run(self, sb: Sandbox) -> tuple[int, str]:
        result = sb.run(LOAD_CMD, timeout_sec=120)
        return result.exit_code, result.stdout

    def destroy(self, sb: Sandbox) -> None:
        sb.close()

    def cleanup_leftovers(self) -> None:
        pass  # every Sandbox is closed by teardown; scratch dir is removed by main()

    def count_leftovers(self) -> int:
        count = 0
        for entry in os.listdir("/proc"):
            if entry.isdigit():
                try:
                    with open(f"/proc/{entry}/comm") as f:
                        count += f.read().strip() == "agentbox-exec"
                except (FileNotFoundError, ProcessLookupError, PermissionError):
                    pass
        return count


class PodmanSystem:
    name = "podman"

    def __init__(self, fixture: str, scratch: str):
        self.fixture = fixture

    def _podman(self, *args: str, timeout: int = 120) -> str:
        proc = subprocess.run(["podman", *args], capture_output=True, text=True, timeout=timeout)
        if proc.returncode != 0:
            raise RuntimeError(f"podman {args[0]} failed: {proc.stderr.strip()[-500:]}")
        return proc.stdout.strip()

    def prepare(self) -> None:
        self._podman("pull", "-q", PODMAN_IMAGE, timeout=600)
        self.cleanup_leftovers()

    def create(self, i: int) -> tuple[str, int]:
        cid = self._podman(
            "run", "-d", "--label", BENCH_LABEL, "--network", "host",
            "-v", f"{self.fixture}:/ws:O", PODMAN_IMAGE, "sleep", "infinity",
        )
        conmon_pid = int(self._podman("inspect", "--format", "{{.State.ConmonPid}}", cid))
        return cid, conmon_pid

    def process_pss(self, handles: list[tuple[str, int]]) -> tuple[int, int]:
        return tree_pss_kb([conmon_pid for _cid, conmon_pid in handles])

    def engine_pss_mb(self) -> float | None:
        return None  # daemonless

    def run(self, handle: tuple[str, int]) -> tuple[int, str]:
        proc = subprocess.run(
            ["podman", "exec", "-w", "/ws", handle[0], "sh", "-c", LOAD_CMD],
            capture_output=True, text=True, timeout=120,
        )
        return proc.returncode, proc.stdout

    # stop -t 0 then rm, rather than `rm -f -t 0`: Podman 3.x's rm has no -t,
    # and `sleep` as PID 1 ignores SIGTERM, so rm -f alone waits 10s each.
    def destroy(self, handle: tuple[str, int]) -> None:
        self._podman("stop", "-t", "0", handle[0])
        self._podman("rm", handle[0])

    def cleanup_leftovers(self) -> None:
        ids = self._podman("ps", "-aq", "--filter", f"label={BENCH_LABEL}").split()
        if ids:
            self._podman("stop", "-t", "0", *ids, timeout=600)
            self._podman("rm", *ids, timeout=600)

    def count_leftovers(self) -> int:
        return len(self._podman("ps", "-aq", "--filter", f"label={BENCH_LABEL}").split())


# Run inside a `--pid=host` helper container, i.e. in Docker's own pid
# namespace, where these pids live. Each container is its init process plus
# its parent containerd-shim; a shim is counted once even if shared.
_DOCKER_PSS_SCRIPT = r"""
total=0; n=0; seen=" "
for p in "$@"; do
  shim=$(awk '{print $4}' /proc/$p/stat 2>/dev/null)
  for q in $shim $p; do
    case "$seen" in *" $q "*) continue ;; esac
    seen="$seen$q "
    k=$(awk '/^Pss:/{print $2}' /proc/$q/smaps_rollup 2>/dev/null)
    if [ -n "$k" ]; then total=$((total + k)); n=$((n + 1)); fi
  done
done
echo $total $n
"""

_DOCKER_ENGINE_SCRIPT = r"""
t=0
for d in /proc/[0-9]*; do
  case "$(cat $d/comm 2>/dev/null)" in
    dockerd|containerd) k=$(awk '/^Pss:/{print $2}' $d/smaps_rollup 2>/dev/null); t=$((t + ${k:-0})) ;;
  esac
done
echo $t
"""


class DockerSystem:
    name = "docker"

    def __init__(self, fixture: str, scratch: str):
        self.fixture = fixture
        self.scratch = scratch

    def _docker(self, *args: str, timeout: int = 120) -> str:
        proc = subprocess.run(["docker", *args], capture_output=True, text=True, timeout=timeout)
        if proc.returncode != 0:
            raise RuntimeError(f"docker {args[0]} failed: {proc.stderr.strip()[-500:]}")
        return proc.stdout.strip()

    # SYS_PTRACE: smaps_rollup of another container's (or dockerd's)
    # processes is ptrace-gated; without it every PSS read is EACCES.
    def _helper(self, script: str, *script_args: str) -> str:
        return self._docker(
            "run", "--rm", "--pid=host", "--cap-add", "SYS_PTRACE", "--label", BENCH_LABEL, PODMAN_IMAGE,
            "sh", "-c", script, "sh", *script_args, timeout=300,
        )

    def prepare(self) -> None:
        # Docker has no equivalent of Podman's `:O` overlay volume, so the
        # fixture is baked into an image instead: each container's own
        # writable layer is then a copy-on-write view over it -- the same
        # isolation the agentbox and Podman setups give.
        ctx = os.path.join(self.scratch, "docker_ctx")
        shutil.copytree(self.fixture, os.path.join(ctx, "fixture"))
        with open(os.path.join(ctx, "Dockerfile"), "w") as f:
            f.write(f"FROM {PODMAN_IMAGE}\nCOPY fixture /ws\n")
        self._docker("build", "-q", "--label", BENCH_LABEL, "-t", DOCKER_IMAGE, ctx, timeout=600)
        self.cleanup_leftovers()

    def create(self, i: int) -> tuple[str, int]:
        cid = self._docker(
            "run", "-d", "--label", BENCH_LABEL, "--network", "host",
            DOCKER_IMAGE, "sleep", "infinity",
        )
        init_pid = int(self._docker("inspect", "--format", "{{.State.Pid}}", cid))
        return cid, init_pid

    def process_pss(self, handles: list[tuple[str, int]]) -> tuple[int, int]:
        kb, nprocs = self._helper(_DOCKER_PSS_SCRIPT, *(str(pid) for _cid, pid in handles)).split()
        return int(kb), int(nprocs)

    def engine_pss_mb(self) -> float | None:
        return int(self._helper(_DOCKER_ENGINE_SCRIPT)) / 1024

    def run(self, handle: tuple[str, int]) -> tuple[int, str]:
        proc = subprocess.run(
            ["docker", "exec", "-w", "/ws", handle[0], "sh", "-c", LOAD_CMD],
            capture_output=True, text=True, timeout=120,
        )
        return proc.returncode, proc.stdout

    def destroy(self, handle: tuple[str, int]) -> None:
        self._docker("rm", "-f", handle[0])  # SIGKILL, no stop timeout

    def cleanup_leftovers(self) -> None:
        ids = self._docker("ps", "-aq", "--filter", f"label={BENCH_LABEL}").split()
        if ids:
            self._docker("rm", "-f", *ids, timeout=600)

    def count_leftovers(self) -> int:
        return len(self._docker("ps", "-aq", "--filter", f"label={BENCH_LABEL}").split())


# ---- one step: N sandboxes of one system --------------------------------

def run_parallel(fn, items, workers: int, deadline: float, min_free_mb: float, phase: str):
    """Runs fn over items with a thread pool; returns (results, per-item seconds).
    Raises StepFailed on any error, low memory, or passing the deadline."""
    def timed(item):
        t0 = time.perf_counter()
        value = fn(item)
        return value, time.perf_counter() - t0

    results, durations = [], []
    failure = None
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(timed, item) for item in items]
        for fut in futures:
            try:
                value, secs = fut.result(timeout=max(1.0, deadline - time.monotonic()))
                results.append(value)
                durations.append(secs)
            except Exception as e:  # noqa: BLE001 -- any failure fails the step
                failure = f"{phase}: {type(e).__name__}: {e}"
            if failure is None and mem_available_mb() < min_free_mb:
                failure = f"{phase}: MemAvailable fell below {min_free_mb:.0f} MB"
            if failure is None and time.monotonic() > deadline:
                failure = f"{phase}: exceeded phase timeout"
            if failure:
                for f in futures:
                    f.cancel()
                break
    # Leaving the with block waited for in-flight work, so every future is
    # now settled: collect everything that succeeded, so the caller can tear
    # down sandboxes created after the failure was noticed.
    if failure:
        partial = [f.result()[0] for f in futures
                   if f.done() and not f.cancelled() and f.exception() is None]
        raise StepFailed(failure, partial)
    return results, durations


def run_step(system, n: int, args) -> dict:
    step = {"system": system.name, "n": n, "ok": True, "failure": None}
    handles = []
    # Background CPU (WSL itself idles at ~0.1 core), subtracted from the
    # idle-with-N-sandboxes reading below.
    busy0 = cpu_busy_jiffies()
    time.sleep(IDLE_CPU_WINDOW_SEC)
    step["baseline_cpu_cores"] = jiffies_to_sec(cpu_busy_jiffies() - busy0) / IDLE_CPU_WINDOW_SEC
    driver_pss_before = pss_kb(os.getpid())
    engine_before = system.engine_pss_mb()
    mem_before = mem_available_mb()
    try:
        # --- create
        t0 = time.perf_counter()
        try:
            handles, create_secs = run_parallel(
                system.create, range(n), args.workers,
                time.monotonic() + args.phase_timeout, args.min_free_mb, "create",
            )
        except StepFailed as e:
            handles = e.partial  # tear down the ones that did get created
            raise
        step["create_total_s"] = time.perf_counter() - t0
        step["create_p50_ms"] = percentile(create_secs, 50) * 1000
        step["create_p95_ms"] = percentile(create_secs, 95) * 1000

        # --- idle footprint
        time.sleep(SETTLE_SEC)
        step["mem_used_mb"] = mem_before - mem_available_mb()
        step["mem_per_sandbox_mb"] = step["mem_used_mb"] / n
        pss, nprocs = system.process_pss(handles)
        step["pss_total_mb"] = pss / 1024
        step["pss_per_sandbox_mb"] = pss / 1024 / n
        step["procs_per_sandbox"] = nprocs / n
        step["driver_pss_delta_mb"] = (pss_kb(os.getpid()) - driver_pss_before) / 1024
        # Shared daemon memory (Docker only): its total, and how much it grew
        # while these N sandboxes were created.
        engine_after = system.engine_pss_mb()
        step["engine_pss_mb"] = engine_after
        step["engine_growth_mb"] = None if engine_after is None else engine_after - engine_before
        busy0 = cpu_busy_jiffies()
        time.sleep(IDLE_CPU_WINDOW_SEC)
        idle = jiffies_to_sec(cpu_busy_jiffies() - busy0) / IDLE_CPU_WINDOW_SEC
        step["idle_cpu_cores"] = idle - step["baseline_cpu_cores"]

        # --- under load: all N run LOAD_CMD at once, args.load_runs times
        loads = []
        for _ in range(args.load_runs):
            busy0 = cpu_busy_jiffies()
            t0 = time.perf_counter()
            outputs, run_secs = run_parallel(
                system.run, handles, n,
                time.monotonic() + args.phase_timeout, args.min_free_mb, "load",
            )
            wall = time.perf_counter() - t0
            bad = [o for o in outputs if o[0] != 0]
            if bad:
                raise StepFailed(f"load: {len(bad)} command(s) exited non-zero, e.g. {bad[0][0]}")
            if len({o[1] for o in outputs}) != 1:
                raise StepFailed("load: sandboxes produced different checksums")
            loads.append({
                "wall_s": wall,
                "cmd_p50_ms": percentile(run_secs, 50) * 1000,
                "cmd_p95_ms": percentile(run_secs, 95) * 1000,
                "cpu_s": jiffies_to_sec(cpu_busy_jiffies() - busy0),
            })
        step["load_runs"] = loads
        step["mem_used_after_load_mb"] = mem_before - mem_available_mb()

    except StepFailed as e:
        step["ok"] = False
        step["failure"] = str(e)
    finally:
        # --- teardown (always, even after a failure)
        t0 = time.perf_counter()
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            list(pool.map(lambda h: _quiet(system.destroy, h), handles))
        step["teardown_total_s"] = time.perf_counter() - t0
        _quiet(system.cleanup_leftovers)
        # Clean-state check: the next step's MemAvailable baseline is only
        # meaningful if nothing from this one is still alive.
        step["leftovers_after_teardown"] = system.count_leftovers()
        if step["leftovers_after_teardown"] and step["ok"]:
            step["ok"] = False
            step["failure"] = f"teardown: {step['leftovers_after_teardown']} sandbox(es) still alive"
    return step


def _quiet(fn, *a):
    try:
        fn(*a)
    except Exception as e:  # noqa: BLE001 -- teardown is best-effort
        print(f"  warning: teardown: {e}", file=sys.stderr)


# ---- reporting -----------------------------------------------------------

def summarize(steps: list[dict]) -> list[dict]:
    """Median (and min/max) per (system, n) across reps, ok steps only."""
    rows = []
    keys = sorted({(s["system"], s["n"]) for s in steps}, key=lambda k: (k[0], k[1]))
    for system, n in keys:
        group = [s for s in steps if s["system"] == system and s["n"] == n]
        ok = [s for s in group if s["ok"]]
        row = {"system": system, "n": n, "reps_ok": len(ok), "reps": len(group),
               "failures": [s["failure"] for s in group if not s["ok"]]}
        metrics = {
            "mem_per_sandbox_mb": lambda s: s["mem_per_sandbox_mb"],
            "pss_per_sandbox_mb": lambda s: s["pss_per_sandbox_mb"],
            "procs_per_sandbox": lambda s: s["procs_per_sandbox"],
            "driver_pss_delta_mb": lambda s: s["driver_pss_delta_mb"],
            "idle_cpu_cores": lambda s: s["idle_cpu_cores"],
            "create_total_s": lambda s: s["create_total_s"],
            "create_p50_ms": lambda s: s["create_p50_ms"],
            "create_p95_ms": lambda s: s["create_p95_ms"],
            "teardown_total_s": lambda s: s["teardown_total_s"],
            "load_wall_s": lambda s: statistics.median(r["wall_s"] for r in s["load_runs"]),
            "load_cmd_p50_ms": lambda s: statistics.median(r["cmd_p50_ms"] for r in s["load_runs"]),
            "load_cmd_p95_ms": lambda s: statistics.median(r["cmd_p95_ms"] for r in s["load_runs"]),
            "load_cpu_s": lambda s: statistics.median(r["cpu_s"] for r in s["load_runs"]),
            "load_cpu_per_cmd_ms": lambda s: statistics.median(r["cpu_s"] for r in s["load_runs"]) / s["n"] * 1000,
            "mem_used_after_load_mb": lambda s: s["mem_used_after_load_mb"],
            "engine_pss_mb": lambda s: s.get("engine_pss_mb"),
            "engine_growth_mb": lambda s: s.get("engine_growth_mb"),
        }
        for name, get in metrics.items():
            vals = [v for v in (get(s) for s in ok) if v is not None]
            if vals:
                row[name] = {"median": statistics.median(vals), "min": min(vals), "max": max(vals)}
        rows.append(row)
    return rows


# The soft "starts to degrade" point: the first N at which a per-sandbox
# cost has more than doubled from its value at the smallest N. PSS stands in
# for memory because MemAvailable is noise at small N; command p95 is
# included even though it mostly measures queueing for CPU cores.
DEGRADE_METRICS = ("pss_per_sandbox_mb", "load_cpu_per_cmd_ms", "load_cmd_p95_ms")


def degrade_points(rows: list[dict]) -> dict:
    points = {}
    for system in sorted({r["system"] for r in rows}):
        ok_rows = sorted((r for r in rows if r["system"] == system and r["reps_ok"]), key=lambda r: r["n"])
        points[system] = {}
        for metric in DEGRADE_METRICS:
            usable = [r for r in ok_rows if metric in r]
            if not usable:
                continue
            base = usable[0][metric]["median"]
            first = next((r["n"] for r in usable[1:] if r[metric]["median"] > 2 * base), None)
            points[system][metric] = {"baseline_n": usable[0]["n"], "baseline": base, "first_n_over_2x": first}
    return points


def print_table(rows: list[dict]) -> None:
    cols = [
        ("mem_per_sandbox_mb", "MB/sbx", "{:7.1f}"),
        ("pss_per_sandbox_mb", "PSS/sbx", "{:7.1f}"),
        ("create_total_s", "create s", "{:8.2f}"),
        ("create_p95_ms", "crt p95", "{:7.0f}"),
        ("idle_cpu_cores", "idle cpu", "{:8.3f}"),
        ("load_wall_s", "load s", "{:7.2f}"),
        ("load_cmd_p95_ms", "cmd p95", "{:7.0f}"),
        ("teardown_total_s", "down s", "{:7.2f}"),
    ]
    print(f"\n{'system':9} {'N':>4} {'ok':>4} " + " ".join(f"{h:>{len(fmt.format(0))}}" for _, h, fmt in cols))
    for r in rows:
        cells = []
        for key, _h, fmt in cols:
            width = len(fmt.format(0))
            cells.append(fmt.format(r[key]["median"]) if key in r else "-".rjust(width))
        print(f"{r['system']:9} {r['n']:>4} {r['reps_ok']}/{r['reps']} " + " ".join(cells))
        for f in r["failures"]:
            print(f"{'':15}failed: {f}")


def machine_info() -> dict:
    with open("/proc/meminfo") as f:
        mem_total = int(f.readline().split()[1]) / 1024 / 1024
    def version(*cmd: str) -> str | None:
        try:
            return subprocess.run(cmd, capture_output=True, text=True).stdout.strip() or None
        except FileNotFoundError:
            return None
    return {
        "kernel": platform.release(),
        "cpus": os.cpu_count(),
        "mem_total_gb": round(mem_total, 1),
        "python": platform.python_version(),
        "podman": version("podman", "--version"),
        "docker": version("docker", "version", "--format", "{{.Server.Version}} ({{.Server.Os}})"),
        "fixture_files": FILE_COUNT,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--steps", default="10,50,100,250,500")
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--systems", default="agentbox,podman")
    parser.add_argument("--workers", type=int, default=16, help="parallel create/teardown workers")
    parser.add_argument("--load-runs", type=int, default=3)
    parser.add_argument("--min-free-mb", type=float, default=1536)
    parser.add_argument("--phase-timeout", type=float, default=300)
    parser.add_argument("--warmup", action="store_true",
                        help="run one discarded step at the largest N per system first, so a "
                             "daemon's (dockerd's) memory reaches its plateau before measuring")
    parser.add_argument("--out", help="write raw steps + summary as JSON here")
    args = parser.parse_args()

    steps_n = [int(x) for x in args.steps.split(",")]
    system_names = args.systems.split(",")

    base = tempfile.mkdtemp(prefix="agentbox_concurrency_")
    fixture = os.path.join(base, "fixture")
    build_fixture(fixture)
    systems = {
        "agentbox": AgentboxSystem(fixture, base),
        "podman": PodmanSystem(fixture, base),
        "docker": DockerSystem(fixture, base),
    }

    info = machine_info()
    print(f"machine: {info}")
    all_steps, warmups = [], []
    failed_at = {}  # system -> smallest n that failed; larger n are skipped
    try:
        for name in system_names:
            systems[name].prepare()
            if args.warmup:
                print(f"warm-up  {name:8} N={max(steps_n):<4} (discarded) ...", end=" ", flush=True)
                step = run_step(systems[name], max(steps_n), args)
                warmups.append(step)
                print("ok" if step["ok"] else f"FAILED: {step['failure']}")
        for rep in range(args.reps):
            # Alternate which system goes first, so slow drift hits both equally.
            order = system_names if rep % 2 == 0 else list(reversed(system_names))
            for n in steps_n:
                for name in order:
                    if name in failed_at and n > failed_at[name]:
                        continue
                    print(f"rep {rep + 1}/{args.reps}  {name:8} N={n:<4} ...", end=" ", flush=True)
                    step = run_step(systems[name], n, args)
                    step["rep"] = rep
                    all_steps.append(step)
                    if step["ok"]:
                        print(f"ok  {step['mem_per_sandbox_mb']:.1f} MB/sandbox, "
                              f"created in {step['create_total_s']:.1f}s")
                    else:
                        print(f"FAILED: {step['failure']}")
                        failed_at[name] = min(n, failed_at.get(name, n))
    finally:
        shutil.rmtree(base, ignore_errors=True)

    rows = summarize(all_steps)
    print_table(rows)
    degrades = degrade_points(rows)
    print("\nfirst N where a per-sandbox cost exceeds 2x its smallest-N value:")
    for system, metrics in degrades.items():
        for metric, p in metrics.items():
            print(f"  {system:9} {metric:22} {p['first_n_over_2x'] or 'never'}")
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"machine": info, "args": vars(args), "summary": rows,
                       "degrade_points": degrades, "steps": all_steps, "warmups": warmups}, f, indent=2)
        print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
