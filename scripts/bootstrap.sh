#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# One-time setup: fetch the QEMU submodule.
#
# QEMU is large; we fetch it shallowly. Re-running is safe and idempotent.

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

cd "${REPO_ROOT}"

# QEMU submodule URL (kept in sync with .gitmodules).
QEMU_URL="https://gitlab.com/qemu-project/qemu.git"

if [ -e "${QEMU_DIR}/configure" ]; then
    log "QEMU submodule already present at ${QEMU_DIR}."
elif git config -f .gitmodules --get submodule.qemu.url >/dev/null 2>&1 \
        && git ls-files --error-unmatch qemu >/dev/null 2>&1; then
    # Submodule is registered in the index; just fetch the recorded gitlink.
    log "Initializing QEMU submodule (this may take a while)..."
    git submodule update --init --depth 1 qemu
else
    # First-time setup: the gitlink isn't in the index yet, so add it shallowly
    # and check out the pinned release for a reproducible build.
    log "Adding QEMU submodule at ${QEMU_PINNED_REF} (this may take a while)..."
    git submodule add --depth 1 --force --branch "${QEMU_PINNED_REF}" \
        "${QEMU_URL}" qemu
fi

# Ensure the submodule sits on the pinned ref even if it was cloned earlier.
if ! git -C "${QEMU_DIR}" describe --tags --exact-match >/dev/null 2>&1; then
    log "Checking out pinned ${QEMU_PINNED_REF}..."
    git -C "${QEMU_DIR}" fetch --depth 1 origin tag "${QEMU_PINNED_REF}" \
        >/dev/null 2>&1 || true
    git -C "${QEMU_DIR}" checkout -q "${QEMU_PINNED_REF}" || \
        warn "Could not check out ${QEMU_PINNED_REF}; continuing with current HEAD."
fi

log "QEMU version:"
git -C "${QEMU_DIR}" describe --tags --always 2>/dev/null || true

log "Bootstrap complete. Next: ./scripts/integrate.sh"
