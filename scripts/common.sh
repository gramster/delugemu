#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared configuration and helpers for the delugemu build scripts.
# Source this from the other scripts: `. "$(dirname "$0")/common.sh"`

set -euo pipefail

# Absolute path to the repository root, regardless of where we're invoked from.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Key directories.
QEMU_DIR="${REPO_ROOT}/qemu"
QEMU_BUILD_DIR="${QEMU_DIR}/build"
SRC_DIR="${REPO_ROOT}/src"

# The qemu-system-arm binary the run helper drives. Defaults to the in-tree
# build, but a packaged/relocatable bundle can point it at its vendored binary
# via DELUGEMU_QEMU_BIN (see scripts/package.sh).
QEMU_SYSTEM_BIN="${DELUGEMU_QEMU_BIN:-${QEMU_BUILD_DIR}/qemu-system-arm}"

# The QEMU machine type registered by our board model. Used by run.sh.
DELUGE_MACHINE="deluge"

# We only build the 32-bit Arm system emulator; the RZ/A1L is a Cortex-A9.
QEMU_TARGETS="arm-softmmu"

# Pinned QEMU release. The build is developed and tested against this tag; bump
# it deliberately and re-test rather than tracking master (whose APIs churn).
QEMU_PINNED_REF="v11.0.1"

# Pretty logging.
log()  { printf '\033[1;34m[delugemu]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[delugemu]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[delugemu]\033[0m %s\n' "$*" >&2; exit 1; }

# Number of parallel build jobs.
if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu)"
else
    JOBS=4
fi

require_submodule() {
    if [ ! -e "${QEMU_DIR}/configure" ]; then
        die "QEMU submodule not found. Run ./scripts/bootstrap.sh first."
    fi
}
