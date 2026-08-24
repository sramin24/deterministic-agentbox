# deterministic-agentbox

A transactional, local-first execution sandbox for AI coding agents.
Zero-copy checkpoint and instant rollback for arbitrary shell commands and
file mutations — on Linux, without Docker, without root for day-to-day
use — by layering OverlayFS mounts inside an unprivileged Linux
namespace.

```
checkpoint(): 0.20 ms       shutil.copytree() snapshot: 22.64 ms
rollback():   0.21 ms       shutil.copytree() restore:  22.36 ms
```
*(measured on a 1,000-file fixture — see `tests/benchmark_latency.py`)*

## Before anything else: platform requirements

**This is a Linux-only project.** `unshare()`, OverlayFS, and `/proc`-based
uid/gid mapping don't exist on macOS or Windows.

- **Not already on Linux?** Use [Lima](https://github.com/lima-vm/lima)
  on macOS (`brew install lima && limactl start --name=agentbox
  template:ubuntu`), not Docker Desktop — Docker's own containerization
  would add a confusing extra layer of namespacing on top of the one this
  project creates. On Apple Silicon with the default `vz` VM type, the
  home-directory `virtiofs` mount is unreliable to write to even after
  `writable: true`; treat the mac-side directory as read-only and `rsync`
  into the VM's native disk before building (`scripts/vm-sync.sh` in this
  repo automates that).
- Install `build-essential`, `fuse-overlayfs`, `uidmap`, `python3-pip`,
  `python3-venv` on the Linux target.

**On Ubuntu 24.04 and later, unprivileged sandboxing is blocked by
default** — a second, independent restriction beyond the usual
`kernel.unprivileged_userns_clone` sysctl. `unshare(CLONE_NEWUSER)`
itself will actually *succeed*; the block surfaces a step later, as
`EPERM` on the `uid_map`/`setgroups` write, once the kernel transitions
the process into a restrictive `unprivileged_userns` AppArmor profile.
The fix is a **one-time, root-required setup step** — after that, every
normal sandbox operation runs fully unprivileged:

```console
$ sudo agentbox setup
✓ AppArmor profile installed for /path/to/agentbox-exec
Sandboxes can now be created without sudo.
```

This grants the permission to exactly one small, dedicated binary
(`agentbox-exec`) — never to your Python interpreter itself, which would
otherwise hand the same privilege to anything else that happens to run
through that same interpreter.

## Install

```console
$ pip install .
$ sudo agentbox setup   # one-time, see above
```

`pip install` compiles the C engine (`make build` under the hood) and
bundles the resulting `libagentbox.so` and `agentbox-exec` into the
installed package — no separate build step needed.

## Quick start: the CLI

```console
$ agentbox run "npm test"
▶ agentbox run: npm test
... live output, exactly as npm test would normally print it ...

✓ exit 0 · 842ms · committed
```

`agentbox run` is a full safety net around one command: it checkpoints
before running, streams the command's real output live (not buffered
until it finishes), and then either commits the change (exit 0) or rolls
it back completely (any failure, or exceeding `--timeout`, default 30s)
— leaving your real files exactly as if the command had never run. Exit
code mirrors the command's own exit code, plus `124` on timeout (matching
GNU `timeout`'s convention), so it's a safe drop-in for existing scripts
and CI:

```console
$ agentbox run --timeout 120 --workspace ./my-project "python setup.py build" && echo ok
```

**Current limitation:** `agentbox run` is deliberately self-contained
because the underlying engine's supervisor — one persistent process per
sandbox, holding the namespace and mount open — only lives as long as
the process that created it. A CLI invocation is a brand-new process
every time, so there's no `agentbox checkpoint` / `agentbox rollback` /
`agentbox diff` as *separate* commands spanning multiple invocations yet
— that needs a persistent, reconnectable daemon this project doesn't
have. Fine-grained checkpoint/rollback/diff control is available today
through the Python API below, where a `Sandbox` object naturally stays
alive across calls in one program.

## Quick start: Python

```python
from agentbox import Sandbox

with Sandbox("/path/to/your/project") as sandbox:
    checkpoint_id = sandbox.checkpoint("before risky refactor")

    result = sandbox.run("some_risky_migration.sh")
    if result.exit_code != 0:
        sandbox.rollback(checkpoint_id)
    else:
        sandbox.commit()
```

Or, using `transaction()` for automatic rollback on any exception:

```python
with Sandbox("/path/to/your/project") as sandbox:
    with sandbox.transaction():
        sandbox.run("rm -rf build/ && ./rebuild.sh")
        result = sandbox.run("./run_tests.sh")
        if result.exit_code != 0:
            raise RuntimeError("tests failed")
    # only reaches here (and stays committed) if nothing raised
```

Live output as a command runs, rather than only after it finishes:

```python
def on_output(is_stderr: bool, chunk: bytes) -> None:
    stream = sys.stderr.buffer if is_stderr else sys.stdout.buffer
    stream.write(chunk)

sandbox.run("npm test", on_output=on_output)
```

## How it works, briefly

```
Python (Sandbox class, ctypes)
   |  agentbox_create() -> forks + execs agentbox-exec, returns a handle
   v
[agentbox-exec supervisor]   (persistent per-Sandbox; holds the user +
                               mount namespaces; an anchor process holds
                               the pid namespace open across commands)
   - unshare(NEWUSER|NEWNS|NEWPID); map uid/gid; MS_PRIVATE
   - mount the OverlayFS layer stack (lowerdir=workspace_dir, ...)
   - serve CHECKPOINT / ROLLBACK / COMMIT / DIFF / RUN / SHUTDOWN
     requests from Python over a private socket
```

Checkpointing never copies files: it seals the current writable overlay
layer in place and starts a fresh one on top, the same mechanism
container image layers use. Rolling back discards layers; committing
walks the whole layer stack (handling deleted files and fully-replaced
directories correctly) and merges it onto the real workspace once.

## Development

```console
$ make build              # compiles out/libagentbox.so and out/agentbox-exec
$ sudo out/agentbox-exec --install-apparmor "$(pwd)/out/agentbox-exec"   # one-time
$ make test-c              # the full C engine test suite, run unprivileged
$ pip install -e '.[test]'
$ pytest tests/
$ python3 tests/benchmark_latency.py
$ python3 examples/agent_simulation.py
```

Never run the functional test suite itself as root — that would mask
real unprivileged-path bugs. Only the one-time setup step needs it.

`scripts/vm-sync.sh` (macOS + Lima only) handles the mac-to-VM sync loop:
`sync` for a one-shot rsync, `watch` to sync on every file change, `shell`
to drop into the VM at the synced directory, `test` to sync and then run
the full C test suite.

## License

Not yet chosen — add one before distributing this beyond local use.
