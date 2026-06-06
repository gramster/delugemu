#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Link the delugemu device models into the upstream QEMU source tree and
# register them with QEMU's Meson + Kconfig build, without hand-editing any
# tracked QEMU file beyond a few clearly-marked, idempotent, reversible hooks.
#
# Strategy
# --------
# QEMU's build is driven by Meson and Kconfig, both organised per-directory:
#
#   * hw/meson.build  enters each hw category with subdir(...). It enters the
#     target dirs (e.g. 'arm') first so that arch source sets like `arm_ss`
#     exist before the device dirs are processed.
#   * hw/Kconfig      sources each category's Kconfig.
#   * Headers live under include/hw/... which is already on the compiler's
#     include path.
#
# So integration does four things, each guarded so re-running is a no-op and
# `--undo` cleanly reverts:
#
#   1. Symlink  src/            -> qemu/hw/deluge          (our .c + meson.build)
#   2. Symlink  src/include/hw/* headers into qemu/include/hw/...  so they
#      resolve on the normal include path with no meson include hacks.
#   3. Append `subdir('deluge')` to qemu/hw/meson.build (at EOF, after the arch
#      dirs, so arm_ss already exists).
#   4. Append `source deluge/Kconfig` to qemu/hw/Kconfig.
#
# Keeping the QEMU-side edits to two appended, marker-guarded lines makes
# rebasing the submodule onto a newer QEMU painless.

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_submodule

LINK_PATH="${QEMU_DIR}/hw/deluge"
HW_MESON="${QEMU_DIR}/hw/meson.build"
HW_KCONFIG="${QEMU_DIR}/hw/Kconfig"
QEMU_INC_HW="${QEMU_DIR}/include/hw"
SRC_INC_HW="${SRC_DIR}/include/hw"

MARKER="# >>> delugemu integration (managed by integrate script) <<<"
MARKER_END="# <<< delugemu integration end >>>"

# Header sub-directories we expose under qemu/include/hw (must match the
# directories present in src/include/hw).
HEADER_DIRS=(arm char display dma gpio misc ssi timer)

undo() {
    log "Removing delugemu integration from the QEMU tree..."

    # 1. Source tree symlink.
    [ -L "${LINK_PATH}" ] && rm -f "${LINK_PATH}"

    # 2. Per-file header symlinks under include/hw/<dir>/.
    local dir hdr target
    for dir in "${HEADER_DIRS[@]}"; do
        [ -d "${SRC_INC_HW}/${dir}" ] || continue
        for hdr in "${SRC_INC_HW}/${dir}"/*.h; do
            [ -e "${hdr}" ] || continue
            target="${QEMU_INC_HW}/${dir}/$(basename "${hdr}")"
            [ -L "${target}" ] && rm -f "${target}"
        done
    done

    # 3 + 4. Remove appended hooks. Use awk (not sed) so marker text containing
    #        regex/delimiter characters is matched literally.
    local f tmp
    for f in "${HW_MESON}" "${HW_KCONFIG}"; do
        if [ -f "${f}" ] && grep -qF "${MARKER}" "${f}"; then
            tmp="${f}.delugemu.tmp"
            awk -v start="${MARKER}" -v end="${MARKER_END}" '
                index($0, start) { skip = 1 }
                skip && index($0, end) { skip = 0; next }
                !skip { print }
            ' "${f}" > "${tmp}" && mv "${tmp}" "${f}"
        fi
    done

    log "Integration removed."
}

if [ "${1:-}" = "--undo" ]; then
    undo
    exit 0
fi

# 1. Symlink our source tree in as hw/deluge.
if [ -L "${LINK_PATH}" ]; then
    log "Symlink ${LINK_PATH} already present."
elif [ -e "${LINK_PATH}" ]; then
    die "${LINK_PATH} exists and is not a symlink; refusing to clobber."
else
    log "Linking ${SRC_DIR} -> ${LINK_PATH}"
    ln -s "${SRC_DIR}" "${LINK_PATH}"
fi

# 2. Symlink headers into qemu/include/hw/<dir>/ (on the default include path).
for dir in "${HEADER_DIRS[@]}"; do
    [ -d "${SRC_INC_HW}/${dir}" ] || continue
    mkdir -p "${QEMU_INC_HW}/${dir}"
    for hdr in "${SRC_INC_HW}/${dir}"/*.h; do
        [ -e "${hdr}" ] || continue
        target="${QEMU_INC_HW}/${dir}/$(basename "${hdr}")"
        if [ -L "${target}" ]; then
            continue
        elif [ -e "${target}" ]; then
            die "${target} exists and is not a symlink; refusing to clobber."
        fi
        ln -s "${hdr}" "${target}"
        log "Linked header $(basename "${hdr}") into include/hw/${dir}/"
    done
done

# 3. Register the meson subdir (idempotent). Appending at EOF guarantees this
#    runs after subdir('arm'), so arm_ss already exists.
if grep -qF "${MARKER}" "${HW_MESON}"; then
    log "hw/meson.build already references delugemu."
else
    log "Registering delugemu subdir in hw/meson.build"
    {
        printf '\n%s\n' "${MARKER}"
        printf "subdir('deluge')\n"
        printf '%s\n' "${MARKER_END}"
    } >> "${HW_MESON}"
fi

# 4. Register the Kconfig (idempotent).
if grep -qF "${MARKER}" "${HW_KCONFIG}"; then
    log "hw/Kconfig already references delugemu."
else
    log "Registering delugemu Kconfig in hw/Kconfig"
    {
        printf '\n%s\n' "${MARKER}"
        printf 'source deluge/Kconfig\n'
        printf '%s\n' "${MARKER_END}"
    } >> "${HW_KCONFIG}"
fi

log "Integration complete. Next: ./scripts/build.sh"
