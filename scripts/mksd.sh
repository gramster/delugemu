#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a raw FAT32 disk image for the Deluge SDHI SD slot from a content
# directory (defaults to ./sdcard, the factory content).
#
# Usage:
#   ./scripts/mksd.sh [content-dir] [output-image]
#
# Defaults:
#   content-dir   sdcard
#   output-image  build/deluge_sd.img
#
# Run the result with:
#   ./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img

set -euo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

CONTENT_DIR="${1:-${REPO_ROOT}/sdcard}"
OUT_IMG="${2:-${REPO_ROOT}/build/deluge_sd.img}"

[ -d "${CONTENT_DIR}" ] || die "Content directory not found: ${CONTENT_DIR}"
mkdir -p "$(dirname "${OUT_IMG}")"

# Size the image at the content size plus 30% slack (min 64 MiB), then round up
# to the next power of two: QEMU's sd-card device requires a power-of-2 capacity.
# Floor at 128 MiB so FAT32 has a reasonably large volume.
content_kib=$(du -sk "${CONTENT_DIR}" | awk '{print $1}')
need_mib=$(( (content_kib * 13 / 10) / 1024 + 64 ))
[ "${need_mib}" -lt 128 ] && need_mib=128
img_mib=128
while [ "${img_mib}" -lt "${need_mib}" ]; do
    img_mib=$(( img_mib * 2 ))
done
log "Content ${content_kib} KiB -> image ${img_mib} MiB (power of 2) at ${OUT_IMG}"

rm -f "${OUT_IMG}"

uname_s="$(uname -s)"
if [ "${uname_s}" = "Darwin" ]; then
    # macOS: create blank image, attach as raw device, format FAT32, copy, eject.
    dd if=/dev/zero of="${OUT_IMG}" bs=1m count="${img_mib}" status=none

    dev="$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage \
            "${OUT_IMG}" | head -1 | awk '{print $1}')"
    [ -n "${dev}" ] || die "hdiutil attach failed"
    # Ensure we always detach, even on error.
    trap 'hdiutil detach "${dev}" >/dev/null 2>&1 || true' EXIT

    newfs_msdos -F 32 -v DELUGE "${dev}" >/dev/null
    diskutil mount "${dev}" >/dev/null
    mp="$(diskutil info "${dev}" | awk -F': *' '/Mount Point/ {print $2}')"
    [ -n "${mp}" ] || die "could not determine mount point for ${dev}"

    log "Copying ${CONTENT_DIR} -> ${mp}"
    # ditto copies directory contents (no extended attrs needed on FAT).
    ditto --norsrc --noextattr --noacl "${CONTENT_DIR}/" "${mp}/"
    sync
    diskutil unmount "${dev}" >/dev/null
    hdiutil detach "${dev}" >/dev/null
    trap - EXIT
else
    # Linux: requires mtools (mkfs.fat + mcopy).
    command -v mkfs.fat >/dev/null || die "mkfs.fat not found (install dosfstools)"
    command -v mcopy    >/dev/null || die "mcopy not found (install mtools)"

    dd if=/dev/zero of="${OUT_IMG}" bs=1M count="${img_mib}" status=none
    mkfs.fat -F 32 -n DELUGE "${OUT_IMG}" >/dev/null
    log "Copying ${CONTENT_DIR} -> image"
    mcopy -i "${OUT_IMG}" -s "${CONTENT_DIR}"/* ::/
fi

log "SD image ready: ${OUT_IMG}"
