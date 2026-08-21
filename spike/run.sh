#!/usr/bin/env bash
# THROWAWAY. Compiles and runs the ns_overlay_probe spike inside the Lima VM,
# against a scratch dir on the VM's native disk (must share a filesystem with
# the overlay dirs it creates under it).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
gcc -std=c11 -Wall -Wextra -O0 -g -o /tmp/ns_overlay_probe ns_overlay_probe.c
rm -rf ~/spike_scratch
mkdir -p ~/spike_scratch
/tmp/ns_overlay_probe ~/spike_scratch
