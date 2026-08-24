"""Benchmark: agentbox's layered checkpoint()/rollback() against a naive
shutil.copytree() full-tree snapshot/restore, on a 1,000-file fixture.

Prints real measured numbers. Deliberately asserts nothing about absolute
timing -- that depends on disk and cache state and will vary by machine;
the comparison between the two approaches is the point, and the numbers
speak for themselves.
"""
import os
import shutil
import tempfile
import time

from agentbox import Sandbox

FILE_COUNT = 1000


def build_fixture(root: str) -> None:
    os.makedirs(root, exist_ok=True)
    for i in range(FILE_COUNT):
        subdir = os.path.join(root, f"dir_{i // 100}")
        os.makedirs(subdir, exist_ok=True)
        with open(os.path.join(subdir, f"file_{i}.txt"), "w") as f:
            f.write(f"content of file {i}\n" * 10)


def benchmark_agentbox(workspace: str) -> tuple[float, float]:
    with Sandbox(workspace) as sb:
        t0 = time.perf_counter()
        ckpt_id = sb.checkpoint()
        checkpoint_ms = (time.perf_counter() - t0) * 1000

        result = sb.run("rm -rf *")  # force a real, full-tree delta to roll back
        assert result.exit_code == 0

        t0 = time.perf_counter()
        sb.rollback(ckpt_id)
        rollback_ms = (time.perf_counter() - t0) * 1000

        check = sb.run("find . -type f | wc -l")
        assert check.stdout.strip() == str(FILE_COUNT), "rollback did not fully restore the fixture"

    return checkpoint_ms, rollback_ms


def benchmark_copytree(workspace: str) -> tuple[float, float]:
    backup = workspace + "_backup"

    t0 = time.perf_counter()
    shutil.copytree(workspace, backup)
    snapshot_ms = (time.perf_counter() - t0) * 1000

    shutil.rmtree(workspace)

    t0 = time.perf_counter()
    shutil.copytree(backup, workspace)
    restore_ms = (time.perf_counter() - t0) * 1000

    shutil.rmtree(backup)
    return snapshot_ms, restore_ms


def main() -> None:
    base = tempfile.mkdtemp(prefix="agentbox_benchmark_")
    ws_agentbox = os.path.join(base, "workspace_agentbox")
    ws_copytree = os.path.join(base, "workspace_copytree")

    print(f"Building two {FILE_COUNT}-file fixture trees (one per approach)...")
    build_fixture(ws_agentbox)
    build_fixture(ws_copytree)

    print("\n--- agentbox: layered checkpoint()/rollback() ---")
    ckpt_ms, rollback_ms = benchmark_agentbox(ws_agentbox)
    print(f"checkpoint(): {ckpt_ms:8.2f} ms")
    print(f"rollback():   {rollback_ms:8.2f} ms")

    print("\n--- naive: shutil.copytree() full-tree snapshot/restore ---")
    snapshot_ms, restore_ms = benchmark_copytree(ws_copytree)
    print(f"copytree() snapshot: {snapshot_ms:8.2f} ms")
    print(f"copytree() restore:  {restore_ms:8.2f} ms")

    print("\n--- comparison ---")
    for label, fast, slow in (
        ("checkpoint vs. snapshot", ckpt_ms, snapshot_ms),
        ("rollback vs. restore", rollback_ms, restore_ms),
    ):
        if fast > 0:
            print(f"{label}: {slow / fast:6.1f}x")
        else:
            print(f"{label}: too fast to measure a ratio ({fast:.3f} ms)")

    shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    main()
