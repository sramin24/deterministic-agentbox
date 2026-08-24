"""The `agentbox` command-line tool.

`agentbox run` is deliberately self-contained: each invocation is its own
process, and the C engine's supervisor (one persistent process per
Sandbox, reachable only over a private socket created at construction
time) doesn't survive past that process exiting. So rather than pretend
separate `checkpoint`/`rollback`/`diff` subcommands work across separate
CLI invocations -- they can't, without a persistent, reconnectable daemon
this project doesn't have yet -- `run` does the whole safe transaction
itself: checkpoint, run the command, commit on success or roll back on
failure/timeout. checkpoint/rollback/diff as fine-grained standalone
operations remain available through the Python API, where a Sandbox
object naturally stays alive across multiple calls in one program.
"""
import argparse
import os
import sys

from . import _core
from .errors import AgentboxError, NamespaceUnavailableError
from .sandbox import Sandbox

_RESET = "\x1b[0m"
_DIM = "\x1b[2m"
_BOLD = "\x1b[1m"
_GREEN = "\x1b[32m"
_RED = "\x1b[31m"
_YELLOW = "\x1b[33m"


def _colorer(enabled: bool):
    return (lambda code, text: f"{code}{text}{_RESET}") if enabled else (lambda code, text: text)


def cmd_setup(args: argparse.Namespace) -> int:
    if os.geteuid() != 0:
        print("agentbox setup must be run as root:\n\n    sudo agentbox setup\n", file=sys.stderr)
        return 1
    try:
        exec_path = _core._find_exec_binary()
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    rc = _core.lib.agentbox_install_apparmor_profile(exec_path.encode())
    if rc == 0:
        print(f"✓ AppArmor profile installed for {exec_path}")
        print("Sandboxes can now be created without sudo.")
        return 0
    detail = _core.lib.agentbox_strerror(rc)
    print(f"✗ setup failed: {detail.decode() if detail else rc}", file=sys.stderr)
    return 1


def cmd_run(args: argparse.Namespace) -> int:
    workspace = os.path.abspath(args.workspace or os.getcwd())
    command_parts = args.command
    if command_parts and command_parts[0] == "--":
        command_parts = command_parts[1:]  # idiomatic explicit end-of-options marker
    command = " ".join(command_parts)
    if not command:
        print("error: no command given", file=sys.stderr)
        return 2

    use_color = sys.stdout.isatty()
    c = _colorer(use_color)

    try:
        sb = Sandbox(workspace)
    except NamespaceUnavailableError as e:
        print(f"{c(_RED, '✗')} {e}", file=sys.stderr)
        return 1

    print(f"{c(_DIM, '▶ agentbox run:')} {command}")

    ckpt_id = sb.checkpoint("cli-run")

    def on_output(is_stderr: bool, data: bytes) -> None:
        stream = sys.stderr.buffer if is_stderr else sys.stdout.buffer
        stream.write(data)
        stream.flush()

    try:
        result = sb.run(command, timeout_sec=args.timeout, on_output=on_output)
    except AgentboxError as e:
        sb.rollback(ckpt_id)
        sb.close()
        print(f"{c(_RED, '✗ agentbox error:')} {e}", file=sys.stderr)
        return 1

    print()
    if result.timed_out:
        sb.rollback(ckpt_id)
        print(f"{c(_YELLOW, f'⏱ timed out after {args.timeout}s')} "
              f"{c(_DIM, '· rolled back, workspace unchanged')}")
        exit_code = 124  # matches the GNU `timeout` command's own convention
    elif result.exit_code == 0:
        sb.commit()
        print(f"{c(_GREEN, '✓ exit 0')} {c(_DIM, f'· {result.duration_ms}ms · committed')}")
        exit_code = 0
    else:
        sb.rollback(ckpt_id)
        print(f"{c(_RED, f'✗ exit {result.exit_code}')} "
              f"{c(_DIM, f'· {result.duration_ms}ms · rolled back, workspace unchanged')}")
        exit_code = result.exit_code

    sb.close()
    return exit_code


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="agentbox", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="subcommand", required=True)

    p_setup = sub.add_parser("setup", help="one-time root setup (installs the AppArmor profile)")
    p_setup.set_defaults(func=cmd_setup)

    p_run = sub.add_parser(
        "run",
        help="run a command in a sandbox: commits on success, rolls back on failure or timeout",
    )
    p_run.add_argument("--timeout", type=int, default=30, metavar="SECONDS")
    p_run.add_argument("--workspace", metavar="DIR", help="defaults to the current directory")
    p_run.add_argument("command", nargs=argparse.REMAINDER,
                        help='the command to run, e.g. agentbox run npm test')
    p_run.set_defaults(func=cmd_run)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
