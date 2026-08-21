#!/usr/bin/env bash
# Dev-only workflow helper for the macOS + Lima setup described in the build
# spec (Part 0.1). Not part of the shipped `agentbox` build or package.
#
# Why this exists: the Lima "agentbox" instance mounts the mac-side repo at
# /Users/savaramin/deterministic-agentbox over virtiofs *read-only*, and the
# home-directory virtiofs mount cannot be made reliably writable on Apple
# Silicon's vz VM type. OverlayFS also needs upperdir/workdir/lowerdir on the
# same filesystem, and trusted.* xattrs (needed for opaque-dir commit merge)
# require a real Linux filesystem, not virtiofs. So every build/test cycle
# rsyncs the read-only mac source into the VM's native ext4 disk first.
set -euo pipefail

INSTANCE="agentbox"
REMOTE_HOME="/home/savaramin.guest"
REMOTE_DIR="${REMOTE_HOME}/work/agentbox"
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RSYNC_EXCLUDES=(--exclude .git --exclude out --exclude __pycache__ --exclude '*.egg-info' --exclude .pytest_cache)

usage() {
  cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  sync    One-shot rsync of the repo from the read-only mac mount into the
          VM's native disk (${REMOTE_DIR}).
  watch   Watch the local repo for changes (via fswatch) and sync on every
          change. Ctrl-C to stop.
  shell   Sync once, then drop into an interactive shell in the VM at the
          synced directory.
  test    Sync once, then run 'make test-c' and 'pytest tests/' inside the
          VM, unprivileged.
EOF
}

do_sync() {
  limactl shell "${INSTANCE}" -- rsync -a --delete "${RSYNC_EXCLUDES[@]}" \
    "/Users/savaramin/deterministic-agentbox/" "${REMOTE_DIR}/"
  echo "synced -> ${INSTANCE}:${REMOTE_DIR}"
}

case "${1:-}" in
  sync)
    do_sync
    ;;
  watch)
    command -v fswatch >/dev/null || { echo "fswatch not found (brew install fswatch)" >&2; exit 1; }
    do_sync
    fswatch -o -l 0.5 --exclude '\.git' "${LOCAL_DIR}" | while read -r _; do
      echo "-- change detected, syncing --"
      do_sync
    done
    ;;
  shell)
    do_sync
    limactl shell "${INSTANCE}" -- bash -lc "cd '${REMOTE_DIR}' && exec bash -l"
    ;;
  test)
    do_sync
    limactl shell "${INSTANCE}" -- bash -lc "cd '${REMOTE_DIR}' && make test-c && python3 -m pytest tests/ -v"
    ;;
  *)
    usage
    exit 1
    ;;
esac
