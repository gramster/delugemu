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
#
# Git hygiene (keeping the submodule's working tree "clean")
# ---------------------------------------------------------
# QEMU has no out-of-tree device build, so the four steps above necessarily
# touch the submodule working tree. To stop that from showing up as a dirty
# submodule (`m qemu`) -- which is easy to stage/commit by accident -- we make
# the changes invisible to git, locally and reversibly:
#
#   a. The untracked symlinks (steps 1-2) are added to the submodule's
#      .git/info/exclude, so `git status` never lists them and they cannot be
#      `git add`ed into the QEMU repo by mistake.
#   b. The two edited tracked files (steps 3-4) are marked `--skip-worktree`,
#      so git ignores our local appends and the submodule reports clean.
#
# Both are local to this checkout (never committed anywhere) and are undone by
# `--undo`. The recorded submodule commit is never changed.

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
HEADER_DIRS=(arm char display dma gpio input misc sd ssi timer usb)

# Tracked upstream files we append our build hooks to. They are kept out of
# `git status` with --skip-worktree (see header comment).
TRACKED_HOOK_FILES=(hw/meson.build hw/Kconfig)

# Absolute path to the submodule's local exclude file (handles the gitdir
# indirection of a submodule, whose .git is a file pointing into .git/modules).
GIT_EXCLUDE="$(cd "${QEMU_DIR}" && git rev-parse --git-path info/exclude 2>/dev/null || true)"
case "${GIT_EXCLUDE}" in
    "") ;;
    /*) ;;
    *) GIT_EXCLUDE="${QEMU_DIR}/${GIT_EXCLUDE}" ;;
esac

# Mark / unmark the appended tracked files skip-worktree so git ignores our
# local edits to them (best-effort; never fatal).
set_skip_worktree() {
    local f
    for f in "${TRACKED_HOOK_FILES[@]}"; do
        git -C "${QEMU_DIR}" update-index --skip-worktree "${f}" 2>/dev/null || true
    done
}
clear_skip_worktree() {
    local f
    for f in "${TRACKED_HOOK_FILES[@]}"; do
        git -C "${QEMU_DIR}" update-index --no-skip-worktree "${f}" 2>/dev/null || true
    done
}

# Add / remove a marker-guarded block in the submodule's .git/info/exclude so
# our injected symlinks never appear as untracked files in the QEMU repo.
write_git_exclude() {
    [ -n "${GIT_EXCLUDE}" ] || return 0
    mkdir -p "$(dirname "${GIT_EXCLUDE}")"
    if grep -qF "${MARKER}" "${GIT_EXCLUDE}" 2>/dev/null; then
        return 0
    fi
    {
        printf '%s\n' "${MARKER}"
        printf '/hw/deluge\n'
        printf '/include/hw/*/rza1l_*.h\n'
        printf '/include/hw/*/deluge_*.h\n'
        printf '%s\n' "${MARKER_END}"
    } >> "${GIT_EXCLUDE}"
    log "Registered injected paths in qemu/.git/info/exclude"
}
strip_git_exclude() {
    [ -n "${GIT_EXCLUDE}" ] && [ -f "${GIT_EXCLUDE}" ] || return 0
    grep -qF "${MARKER}" "${GIT_EXCLUDE}" || return 0
    local tmp="${GIT_EXCLUDE}.delugemu.tmp"
    awk -v start="${MARKER}" -v end="${MARKER_END}" '
        index($0, start) { skip = 1 }
        skip && index($0, end) { skip = 0; next }
        !skip { print }
    ' "${GIT_EXCLUDE}" > "${tmp}" && mv "${tmp}" "${GIT_EXCLUDE}"
}

undo() {
    log "Removing delugemu integration from the QEMU tree..."

    # 0. Let git track our hook files again before we revert their contents,
    #    so the reverted (== upstream) files show up as clean.
    clear_skip_worktree

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
            # Strip the marker block AND the single blank line our append
            # prepended before it, so the file is byte-identical to upstream.
            # Blank lines are buffered and only emitted once a non-blank,
            # non-marker line follows; the buffer is discarded at the start
            # marker (dropping our separator) and flushed at EOF (preserving
            # any legitimate trailing blanks).
            awk -v start="${MARKER}" -v end="${MARKER_END}" '
                index($0, start) { hold = ""; skip = 1; next }
                skip && index($0, end) { skip = 0; next }
                skip { next }
                $0 == "" { hold = hold $0 ORS; next }
                { printf "%s", hold; hold = ""; print }
                END { printf "%s", hold }
            ' "${f}" > "${tmp}" && mv "${tmp}" "${f}"
        fi
    done

    # 5. Drop the .git/info/exclude block for our injected symlinks.
    strip_git_exclude

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

# 5. Keep the injected symlinks out of the submodule's `git status`.
write_git_exclude

# 6. Hide our appends to the tracked hook files so the submodule stays clean
#    and the edits can never be staged/committed by accident.
set_skip_worktree

log "Integration complete. Next: ./scripts/build.sh"
log "The qemu submodule now reports clean (git status -C qemu); run"
log "  ./scripts/integrate.sh --undo  to fully revert these local changes."
