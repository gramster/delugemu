#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# One-time setup: fetch the QEMU submodule.
#
# QEMU is large; we fetch it shallowly. Re-running is safe and idempotent.

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

cd "${REPO_ROOT}"

if [ -e "${QEMU_DIR}/configure" ]; then
    log "QEMU submodule already present at ${QEMU_DIR}."
else
    log "Initializing QEMU submodule (this may take a while)..."
    git submodule update --init --depth 1 qemu
fi

log "QEMU version:"
git -C "${QEMU_DIR}" describe --tags --always 2>/dev/null || true

log "Bootstrap complete. Next: ./scripts/integrate.sh"
