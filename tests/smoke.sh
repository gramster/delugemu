#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Smoke test: confirm the build produced qemu-system-arm and that the Deluge
# machine type is registered. This is the M0 acceptance check; it does not yet
# boot firmware (see docs/roadmap.md).
#
# Exit non-zero on failure so it can gate CI.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/qemu/build/qemu-system-arm"

fail() { printf 'SMOKE FAIL: %s\n' "$*" >&2; exit 1; }

[ -x "${BIN}" ] || fail "qemu-system-arm not built at ${BIN} (run scripts/build.sh)"

if ! "${BIN}" -M help | grep -qi '^deluge'; then
    fail "machine 'deluge' is not registered in -M help"
fi

# The machine should be inspectable without a firmware image.
"${BIN}" -M deluge -S -display none -monitor none -serial none \
    -no-shutdown -snapshot </dev/null &
qemu_pid=$!
sleep 1
if ! kill -0 "${qemu_pid}" 2>/dev/null; then
    fail "qemu-system-arm exited immediately when starting the deluge machine"
fi
kill "${qemu_pid}" 2>/dev/null || true
wait "${qemu_pid}" 2>/dev/null || true

printf 'SMOKE OK: deluge machine present and instantiable\n'
