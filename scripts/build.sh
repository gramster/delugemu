#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configure (if needed) and build qemu-system-arm with the Deluge machine.
#
# Usage:
#   ./scripts/build.sh            # incremental build
#   ./scripts/build.sh --reconfigure
#   ./scripts/build.sh --debug    # configure a debug build (implies reconfigure)

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_submodule

# Ensure our models are linked in before configuring.
if [ ! -L "${QEMU_DIR}/hw/deluge" ]; then
    warn "delugemu sources not linked into QEMU; running integrate.sh first."
    "${REPO_ROOT}/scripts/integrate.sh"
fi

RECONFIGURE=0
# Initialised empty; under `set -u` on bash 3.2 (macOS) an empty array must be
# expanded with the `${arr[@]+...}` guard, used below.
EXTRA_CONFIGURE_ARGS=()

for arg in "$@"; do
    case "${arg}" in
        --reconfigure) RECONFIGURE=1 ;;
        --debug)
            RECONFIGURE=1
            EXTRA_CONFIGURE_ARGS+=(--enable-debug)
            ;;
        *) EXTRA_CONFIGURE_ARGS+=("${arg}") ;;
    esac
done

cd "${QEMU_DIR}"

if [ "${RECONFIGURE}" -eq 1 ] || [ ! -f "${QEMU_BUILD_DIR}/build.ninja" ]; then
    log "Configuring QEMU (targets: ${QEMU_TARGETS})..."
    mkdir -p "${QEMU_BUILD_DIR}"
    cd "${QEMU_BUILD_DIR}"
    ../configure \
        --target-list="${QEMU_TARGETS}" \
        --disable-werror \
        ${EXTRA_CONFIGURE_ARGS[@]+"${EXTRA_CONFIGURE_ARGS[@]}"}
    cd "${QEMU_DIR}"
fi

log "Building with ${JOBS} jobs..."
ninja -C "${QEMU_BUILD_DIR}"

BIN="${QEMU_BUILD_DIR}/qemu-system-arm"
if [ -x "${BIN}" ]; then
    log "Build complete: ${BIN}"
    log "Verify the Deluge machine is registered:"
    "${BIN}" -M help | grep -i deluge || \
        warn "Deluge machine not listed yet (expected until the board model is implemented)."
else
    die "Build finished but ${BIN} was not produced."
fi
