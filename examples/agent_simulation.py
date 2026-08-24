"""Runnable simulation: an autonomous agent attempts a multi-file fix,
its own test step fails, it rolls back, and retries successfully --
printing timing at each stage.
"""
import os
import shutil
import tempfile
import time

from agentbox import Sandbox


def setup_fixture_project(root: str) -> None:
    os.makedirs(os.path.join(root, "src"), exist_ok=True)
    with open(os.path.join(root, "src", "calculator.py"), "w") as f:
        f.write(
            "def add(a, b):\n"
            "    return a + b\n"
            "\n"
            "def divide(a, b):\n"
            "    return a / b  # BUG: the agent is asked to add a zero-check\n"
        )
    with open(os.path.join(root, "test_calculator.py"), "w") as f:
        f.write(
            "import sys\n"
            "sys.path.insert(0, 'src')\n"
            "from calculator import add, divide\n"
            "\n"
            "assert add(2, 3) == 5\n"
            "try:\n"
            "    divide(1, 0)\n"
            "    print('FAIL: expected ValueError to be raised')\n"
            "    sys.exit(1)\n"
            "except ValueError:\n"
            "    print('PASS: divide by zero handled correctly')\n"
        )


def attempt_1_wrong_exception_type(sandbox: Sandbox) -> None:
    """The agent's first attempt: adds a zero-check, but raises the wrong
    exception type -- a plausible, realistic mistake."""
    sandbox.run(
        "cat > src/calculator.py <<'PY'\n"
        "def add(a, b):\n"
        "    return a + b\n"
        "\n"
        "def divide(a, b):\n"
        "    if b == 0:\n"
        "        raise RuntimeError('cannot divide by zero')  # wrong type\n"
        "    return a / b\n"
        "PY\n"
    )


def attempt_2_informed_by_failure(sandbox: Sandbox) -> None:
    """The agent's second attempt, informed by attempt 1's test failure:
    raises the exception type the test actually expects."""
    sandbox.run(
        "cat > src/calculator.py <<'PY'\n"
        "def add(a, b):\n"
        "    return a + b\n"
        "\n"
        "def divide(a, b):\n"
        "    if b == 0:\n"
        "        raise ValueError('cannot divide by zero')\n"
        "    return a / b\n"
        "PY\n"
    )


def run_test_suite(sandbox: Sandbox):
    return sandbox.run("python3 test_calculator.py")


def _print_test_output(result) -> None:
    # A wrong exception type crashes the test script with an uncaught
    # traceback on stderr before any print() runs -- stdout is genuinely
    # empty in that case, not a bug. Show stderr too so the failure reads
    # clearly instead of just an empty string.
    if result.stdout.strip():
        print(f"test output:   {result.stdout.strip()!r}")
    else:
        print(f"test output:   (empty stdout, exit_code={result.exit_code})")
        if result.stderr.strip():
            print(f"test stderr:   {result.stderr.strip().splitlines()[-1]!r}")


def main() -> None:
    workspace = tempfile.mkdtemp(prefix="agentbox_example_")
    setup_fixture_project(workspace)
    print(f"Fixture project created at {workspace}\n")

    t_start = time.perf_counter()
    with Sandbox(workspace) as sandbox:
        print("=== Attempt 1 ===")
        t0 = time.perf_counter()
        checkpoint_id = sandbox.checkpoint("before-attempt-1")
        print(f"checkpoint():  {(time.perf_counter() - t0) * 1000:.2f} ms  (id={checkpoint_id})")

        t0 = time.perf_counter()
        attempt_1_wrong_exception_type(sandbox)
        print(f"apply fix:     {(time.perf_counter() - t0) * 1000:.2f} ms")

        t0 = time.perf_counter()
        result = run_test_suite(sandbox)
        print(f"run tests:     {(time.perf_counter() - t0) * 1000:.2f} ms")
        _print_test_output(result)

        if result.exit_code != 0:
            print("Tests FAILED. Rolling back attempt 1...")
            t0 = time.perf_counter()
            sandbox.rollback(checkpoint_id)
            print(f"rollback():    {(time.perf_counter() - t0) * 1000:.2f} ms")

            print("\n=== Attempt 2 (informed by attempt 1's failure) ===")
            t0 = time.perf_counter()
            attempt_2_informed_by_failure(sandbox)
            print(f"apply fix:     {(time.perf_counter() - t0) * 1000:.2f} ms")

            t0 = time.perf_counter()
            result = run_test_suite(sandbox)
            print(f"run tests:     {(time.perf_counter() - t0) * 1000:.2f} ms")
            print(f"test output:   {result.stdout.strip()!r}")

        if result.exit_code == 0:
            print("\nTests PASSED. Committing.")
            t0 = time.perf_counter()
            sandbox.commit()
            print(f"commit():      {(time.perf_counter() - t0) * 1000:.2f} ms")
        else:
            print("\nTests still failing after retry -- leaving the real workspace unchanged.")

    total_ms = (time.perf_counter() - t_start) * 1000
    print(f"\nTotal simulation time: {total_ms:.2f} ms")

    with open(os.path.join(workspace, "src", "calculator.py")) as f:
        print("\nFinal committed src/calculator.py:")
        print(f.read())

    shutil.rmtree(workspace, ignore_errors=True)


if __name__ == "__main__":
    main()
