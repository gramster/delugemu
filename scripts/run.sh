#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run Deluge firmware under the emulator.
#
# Usage:
#   ./scripts/run.sh [firmware.elf|firmware.bin] [options] [-- qemu args...]
#
# The firmware image is optional. If omitted, run.sh uses a .bin (or .elf) image
# found in the firmware folder (default: <repo>/firmware; override with the
# DELUGEMU_FIRMWARE_DIR environment variable). If that folder has no image and
# the terminal is interactive, run.sh offers to download the Deluge community
# firmware release from Synthstrom, unzips it into the firmware folder, and uses
# it from then on.
#
# Options:
#   --sd <path>             Attach an SD card to the SDHI slot. <path> may be:
#                             * a raw FAT disk image file (attached directly), or
#                             * a directory, which is snapshotted into a
#                               temporary FAT image at launch. If the directory
#                               name ends in '_rw', changes the guest makes to
#                               the card are written back to the directory when
#                               the emulator exits; otherwise the directory is
#                               left untouched (read-only snapshot).
#                           If omitted, run.sh auto-detects an 'sdcard_rw' or
#                           'sdcard' directory in the current working directory
#                           and uses it (the '_rw' variant takes precedence). If
#                           neither exists and the terminal is interactive,
#                           run.sh offers to download the Synthstrom factory card
#                           contents into ./sdcard and uses that.
#   --midi <chardev>        Back SCIF0 (the DIN MIDI UART) with a QEMU chardev
#                           spec, e.g. --midi udp:127.0.0.1:1999 or --midi pty.
#                           Pass the special value 'coremidi' to expose the
#                           Deluge's DIN MIDI as a real host MIDI in/out port
#                           named "DelugEmu DIN" (macOS). When omitted, SCIF0 is
#                           multiplexed with the monitor on stdio.
#   --usb-midi <chardev>    Attach a host USB-MIDI device backed by a QEMU
#                           chardev spec, e.g. --usb-midi udp:127.0.0.1:1998 or
#                           --usb-midi pty. Pass 'coremidi' to expose it as a
#                           real host MIDI in/out port named "DelugEmu USB"
#                           (macOS). Routed to serial slot 1.
#   --audio <driver>        Select a non-default host audio backend for the
#                           SSIF (I2S) output (-audiodev <driver>). Audio is on
#                           by default using the OS default backend (coreaudio
#                           on macOS, pa on Linux, dsound on Windows), so this
#                           flag is only needed to override it, e.g. --audio
#                           sdl / wav / none. Use 'auto' to force the OS
#                           default explicitly. Play a note in an instrument
#                           clip to hear 44.1 kHz stereo output.
#   --audio-buffer <ms>     Output buffer cushion in milliseconds (default 15).
#                           Raise it if you hear dropouts; lower it to trim the
#                           delay when playing the emulated Deluge live from
#                           external MIDI.
#   --tx-render-head <addr|auto>
#                           Advanced: bound audio ring reads by the firmware's
#                           render head (AudioEngine::i2sTXBufferPos) at this
#                           guest address (hex, e.g. 0x20038fdc), eliminating
#                           the ring-lap distortion under heavy load. The
#                           address is firmware-build specific; with it unset
#                           (default) reads track the DMA play head, which is
#                           firmware-independent but can distort under load.
#                           Pass 'auto' to have the emulator locate the render
#                           head itself by scanning guest RAM (best-effort; for
#                           stripped firmware with no symbols).
#   --display <mode>        Display mode:
#                             console   open the front-panel skin window with
#                                       the modelled OLED / pad-grid / 7-seg
#                                       overlays (default)
#                             headless  no GUI; serial console + monitor muxed
#                                       on stdio (Ctrl-A C switches)
#                             none      graphics subsystem present but not shown
#   --skin-scale <pct|auto|native>
#                           Front-panel window size. The native panel is
#                           2256x1584; on a HiDPI/Retina display the host shows
#                           that surface at half its pixel count in points, a
#                           well-sized, crisp window, so 'native' (default) opens
#                           at full resolution with no down-sampling. On a
#                           low-DPI monitor where native overflows, pass 'auto'
#                           to detect the primary monitor and pick the largest
#                           scale that fits within ~90% of it, or give an
#                           explicit percent 10-100. The View > Zoom-to-fit menu
#                           item makes the window resizable on demand.
#   --skin-refresh-ms <ms>  Front-panel UI refresh interval (default 50, ~20fps).
#                           Raise it to lower the host redraw rate and free the
#                           main loop for audio under heavy live load; lower it
#                           for a smoother panel on a fast host. Range 10-1000.
#   --icount [<shift>]      Run the CPU on a deterministic instruction-counted
#                           virtual clock locked to real time (-icount
#                           shift=<shift>,sleep=on). This makes audio internally
#                           consistent (no stale-ring distortion or dropouts)
#                           by pacing the guest to the virtual clock, but it
#                           also caps the guest to <= real time, so under heavy
#                           DSP load playback runs slow (lower pitch/tempo)
#                           rather than breaking up. Best for offline/clean
#                           capture, not live external-MIDI play. <shift> is the
#                           ns-per-instruction power of two; default 'auto' lets
#                           QEMU tune it. Off by default.
#   --monitor               Expose the interactive QEMU monitor on stdio. Off by
#                           default, so a plain run shows only the serial console
#                           and does not drop you at a '(qemu)' prompt; pass this
#                           to drive the machine from the monitor.
#   --inverse               Use the original dark front-panel skin
#                           (Delugemu_Inverse.png) with black unlit pads. The
#                           default is the light skin (Delugemu_Normal.png) with
#                           white unlit pads; pad illumination is identical in
#                           both. Ignored if DELUGEMU_SKIN is set.
#   -h, --help              Show this help and exit.
#
# Anything after a literal `--`, or any unrecognised flag, is passed straight
# through to qemu-system-arm. Useful extras:
#   -d guest_errors,unimp   log unimplemented device accesses
#   -S -s                   freeze at reset and wait for gdb on :1234

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    # Print the banner comment (from the "Run Deluge..." line through the end of
    # the leading comment block), stripping the leading "# ".
    awk 'NR>=4 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
}

# Resolve a --sd argument that may be either a raw image file or a directory.
# A directory is snapshotted into a temporary FAT image (via mksd.sh) so the
# guest sees a normal card. If its name ends in '_rw', guest changes are copied
# back to the directory on exit (see sd_writeback and the cleanup trap).
sd_setup() {
    local path="$1"
    if [ -d "${path}" ]; then
        SD_FOLDER="${path%/}"
        SD_TMP_IMG="$(mktemp "${TMPDIR:-/tmp}/delugemu-sd.XXXXXX")"
        log "Snapshotting folder into a temporary SD image: ${SD_FOLDER}"
        "${REPO_ROOT}/scripts/mksd.sh" "${SD_FOLDER}" "${SD_TMP_IMG}" \
            || die "failed to build SD image from ${SD_FOLDER}"
        case "${SD_FOLDER}" in
            *_rw)
                SD_WRITEBACK=1
                log "Folder name ends in '_rw': changes will be written back on exit"
                ;;
        esac
        SD_ARGS=(-drive "if=sd,format=raw,file=${SD_TMP_IMG}")
    elif [ -f "${path}" ]; then
        SD_ARGS=(-drive "if=sd,format=raw,file=${path}")
    else
        die "SD image/directory not found: ${path}"
    fi
}

# Mirror the (possibly guest-modified) temporary SD image back into the source
# directory. Only called for '_rw' folders. Best-effort: warns on failure
# rather than aborting, since the emulator has already run.
sd_writeback() {
    [ -n "${SD_TMP_IMG}" ] || return 0
    [ -d "${SD_FOLDER}" ] || return 0
    log "Writing SD changes back to ${SD_FOLDER}"
    case "$(uname -s)" in
        Darwin)
            local dev mp
            dev="$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage \
                    "${SD_TMP_IMG}" 2>/dev/null | head -1 | awk '{print $1}')"
            if [ -z "${dev}" ]; then
                warn "could not attach SD image for write-back; ${SD_FOLDER} left unchanged"
                return 0
            fi
            if ! diskutil mount "${dev}" >/dev/null 2>&1; then
                warn "could not mount SD image for write-back; ${SD_FOLDER} left unchanged"
                hdiutil detach "${dev}" >/dev/null 2>&1 || true
                return 0
            fi
            mp="$(diskutil info "${dev}" | awk -F': *' '/Mount Point/ {print $2}')"
            if [ -n "${mp}" ]; then
                rsync -a --delete \
                    --exclude '.fseventsd' --exclude '.Spotlight-V100' \
                    --exclude '.Trashes' --exclude '.TemporaryItems' \
                    "${mp}/" "${SD_FOLDER}/" \
                    || warn "write-back to ${SD_FOLDER} reported errors"
            else
                warn "could not locate mount point for write-back"
            fi
            diskutil unmount "${dev}" >/dev/null 2>&1 || true
            hdiutil detach "${dev}" >/dev/null 2>&1 || true
            ;;
        *)
            if ! command -v mcopy >/dev/null 2>&1; then
                warn "mtools (mcopy) not found; cannot write SD changes back to ${SD_FOLDER}"
                return 0
            fi
            local tmpdir
            tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/delugemu-sdout.XXXXXX")"
            if mcopy -i "${SD_TMP_IMG}" -s -n -m "::/*" "${tmpdir}/" 2>/dev/null; then
                if command -v rsync >/dev/null 2>&1; then
                    rsync -a --delete "${tmpdir}/" "${SD_FOLDER}/" \
                        || warn "write-back to ${SD_FOLDER} reported errors"
                else
                    cp -a "${tmpdir}/." "${SD_FOLDER}/" \
                        || warn "write-back to ${SD_FOLDER} reported errors"
                fi
            else
                warn "could not read SD image for write-back; ${SD_FOLDER} left unchanged"
            fi
            rm -rf "${tmpdir}"
            ;;
    esac
    return 0
}

# Where auto-detected and downloaded firmware images live. Overridable so a user
# can point at an existing collection without copying it in.
FIRMWARE_DIR="${DELUGEMU_FIRMWARE_DIR:-${REPO_ROOT}/firmware}"

# The community firmware release offered when no image is present. This is the
# unmodified open-source build published by Synthstrom; we only download and
# unzip it, we do not redistribute it.
COMMUNITY_FW_URL="https://github.com/SynthstromAudible/DelugeFirmware/releases/download/release_1_2_1/deluge-community-release-1_2_1.zip"
COMMUNITY_FW_NAME="Deluge community firmware 1.2.1 (Chopin)"

# The Synthstrom factory SD-card contents offered when no card is given and no
# default sdcard folder exists. Downloaded and unzipped into SD_DIR, not
# redistributed. SD_DIR (overridable) is also the folder auto-detected below.
FACTORY_SD_URL="https://synthstrom-audible-deluge.s3.us-east-2.amazonaws.com/Deluge+OLED+V4p1p0+factory+card+contents.zip"
FACTORY_SD_NAME="Deluge OLED V4.1.0 factory card contents"
SD_DIR="${DELUGEMU_SD_DIR:-sdcard}"

# Echo the path of a usable firmware image already present in FIRMWARE_DIR (or
# nothing). Prefer an OLED .bin (the emulator's OLED can also render the 7-seg
# UI), then any .bin, then an .elf, so an existing firmware/deluge.elf dev setup
# keeps working too. `|| true` guards against SIGPIPE under `set -o pipefail`.
find_local_firmware() {
    [ -d "${FIRMWARE_DIR}" ] || return 0
    local f
    f="$(find "${FIRMWARE_DIR}" -type f -iname '*.bin' ! -iname '*7seg*' \
            2>/dev/null | LC_ALL=C sort | head -n1 || true)"
    [ -n "${f}" ] || f="$(find "${FIRMWARE_DIR}" -type f -iname '*.bin' \
            2>/dev/null | LC_ALL=C sort | head -n1 || true)"
    [ -n "${f}" ] || f="$(find "${FIRMWARE_DIR}" -type f -iname '*.elf' \
            2>/dev/null | LC_ALL=C sort | head -n1 || true)"
    [ -n "${f}" ] && printf '%s\n' "${f}"
    return 0
}

# Download a zip archive and unzip it into a directory. Uses curl or wget to
# fetch and unzip or python's zipfile module to extract, so it works on macOS,
# Linux and Windows/MSYS2 without extra tooling.
download_and_unzip() {
    local url="$1" dir="$2" name="$3" tmp zip
    mkdir -p "${dir}"
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/delugemu-dl.XXXXXX")" || die "mktemp failed"
    zip="${tmp}/archive.zip"

    log "Downloading ${name}..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL --progress-bar -o "${zip}" "${url}" \
            || { rm -rf "${tmp}"; die "download failed (curl): ${url}"; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "${zip}" "${url}" \
            || { rm -rf "${tmp}"; die "download failed (wget): ${url}"; }
    else
        rm -rf "${tmp}"
        die "neither curl nor wget found; cannot download ${name}"
    fi

    log "Extracting into ${dir}"
    if command -v unzip >/dev/null 2>&1; then
        unzip -o -q "${zip}" -d "${dir}" \
            || { rm -rf "${tmp}"; die "failed to unzip archive"; }
    elif command -v python3 >/dev/null 2>&1; then
        python3 -m zipfile -e "${zip}" "${dir}" \
            || { rm -rf "${tmp}"; die "failed to extract archive (python3)"; }
    elif command -v python >/dev/null 2>&1; then
        python -m zipfile -e "${zip}" "${dir}" \
            || { rm -rf "${tmp}"; die "failed to extract archive (python)"; }
    else
        rm -rf "${tmp}"
        die "no unzip or python found to extract the archive"
    fi
    rm -rf "${tmp}"
}

BIN="${QEMU_SYSTEM_BIN}"
[ -x "${BIN}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

# Firmware is optional. Treat the first argument as a firmware path only when it
# is present and not an option (an option, or nothing, means "auto-detect").
FIRMWARE=""
case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    ""|-*) ;;
    *) FIRMWARE="$1"; shift ;;
esac

# Resolve the firmware image when none was given on the command line: use one
# from the firmware folder, or offer to download the community release.
if [ -z "${FIRMWARE}" ]; then
    FIRMWARE="$(find_local_firmware)"
    if [ -n "${FIRMWARE}" ]; then
        log "No firmware given; using ${FIRMWARE#"${REPO_ROOT}/"}"
    elif [ -t 0 ]; then
        printf '%s\n' "No firmware specified and none found in ${FIRMWARE_DIR}." >&2
        printf '%s' "Download ${COMMUNITY_FW_NAME} (~900 KB) from Synthstrom and use it? [y/N] " >&2
        read -r reply || reply=""
        case "${reply}" in
            [yY]|[yY][eE][sS])
                download_and_unzip "${COMMUNITY_FW_URL}" "${FIRMWARE_DIR}" "${COMMUNITY_FW_NAME}"
                FIRMWARE="$(find_local_firmware)"
                [ -n "${FIRMWARE}" ] \
                    || die "firmware download/extract produced no .bin image"
                log "Using ${FIRMWARE#"${REPO_ROOT}/"}"
                ;;
            *)
                usage
                die "no firmware. Pass a firmware path or place a .bin in ${FIRMWARE_DIR}."
                ;;
        esac
    else
        usage
        die "no firmware given and none found in ${FIRMWARE_DIR} (run interactively to fetch the community firmware, or pass a firmware path)"
    fi
fi

[ -f "${FIRMWARE}" ] || die "Firmware not found: ${FIRMWARE}"

MIDI=""
USB_MIDI=""
AUDIO=""
AUDIO_BUFFER=""
TX_RENDER_HEAD=""
DISPLAY_MODE="console"
ICOUNT=""
MONITOR=0
INVERSE=0
# Front-panel window scale. Default to the native panel resolution (no
# down-scale): it composites the skin at full 2256x1584 so the OLED and pads
# stay crisp, and on a HiDPI/Retina display the host shows that surface at half
# its pixel count in points, i.e. a well-sized window. 'auto' (opt-in) probes
# the monitor and picks a fitting percent; an explicit percent forces a scale.
SKIN_SCALE="native"
# Front-panel UI refresh interval in milliseconds (empty = device default,
# currently 50ms / 20fps). A longer interval lowers the host redraw rate, which
# reduces the main-loop/BQL contention that competes with audio under live load;
# a shorter one makes the panel smoother on a fast host.
SKIN_REFRESH_MS=""
# Host-side playback buffer for the audio backend, in microseconds. On Windows
# the dsound voice is serviced from QEMU's main loop, the same thread that
# recomposites the front-panel skin; a periodic full-frame skin upload can
# briefly stall that service, so the host buffer must hold enough audio for the
# OS DMA to ride the stall without a gap (80 ms). macOS (coreaudio) and Linux
# (pa) service playback on their own callback/thread, independent of QEMU's main
# loop, so a much smaller buffer rides skin stalls fine while cutting the
# perceived press->sound latency; default to 30 ms there. This is independent of
# the device's own staging-FIFO cushion (prime-ms). Overridable via
# DELUGEMU_AUDIO_HOST_BUFFER_US for experimentation.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) _audio_host_buffer_default=80000 ;;
    *)                    _audio_host_buffer_default=30000 ;;
esac
AUDIO_HOST_BUFFER_US="${DELUGEMU_AUDIO_HOST_BUFFER_US:-${_audio_host_buffer_default}}"

SD_ARGS=()
SD_FOLDER=""
SD_TMP_IMG=""
SD_WRITEBACK=0
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --)
            shift
            EXTRA_ARGS+=("$@")
            break
            ;;
        --sd)
            [ -n "${2:-}" ] || die "--sd requires an image path or directory"
            sd_setup "$2"
            shift 2
            ;;
        --sd=*)
            sd_setup "${1#--sd=}"
            shift
            ;;
        --midi)
            [ -n "${2:-}" ] || die "--midi requires a chardev spec"
            MIDI="$2"; shift 2
            ;;
        --midi=*) MIDI="${1#--midi=}"; shift ;;
        --usb-midi)
            [ -n "${2:-}" ] || die "--usb-midi requires a chardev spec"
            USB_MIDI="$2"; shift 2
            ;;
        --usb-midi=*) USB_MIDI="${1#--usb-midi=}"; shift ;;
        --audio)
            [ -n "${2:-}" ] || die "--audio requires a driver name"
            AUDIO="$2"; shift 2
            ;;
        --audio=*) AUDIO="${1#--audio=}"; shift ;;
        --audio-buffer)
            [ -n "${2:-}" ] || die "--audio-buffer requires a value in ms"
            AUDIO_BUFFER="$2"; shift 2
            ;;
        --audio-buffer=*) AUDIO_BUFFER="${1#--audio-buffer=}"; shift ;;
        --tx-render-head)
            [ -n "${2:-}" ] || die "--tx-render-head requires a guest address"
            TX_RENDER_HEAD="$2"; shift 2
            ;;
        --tx-render-head=*) TX_RENDER_HEAD="${1#--tx-render-head=}"; shift ;;
        --display)
            [ -n "${2:-}" ] || die "--display requires a mode"
            DISPLAY_MODE="$2"; shift 2
            ;;
        --display=*) DISPLAY_MODE="${1#--display=}"; shift ;;
        --skin-scale)
            [ -n "${2:-}" ] || die "--skin-scale requires a value (percent, 'auto', or 'native')"
            SKIN_SCALE="$2"; shift 2
            ;;
        --skin-scale=*) SKIN_SCALE="${1#--skin-scale=}"; shift ;;
        --skin-refresh-ms)
            [ -n "${2:-}" ] || die "--skin-refresh-ms requires a value in ms"
            SKIN_REFRESH_MS="$2"; shift 2
            ;;
        --skin-refresh-ms=*) SKIN_REFRESH_MS="${1#--skin-refresh-ms=}"; shift ;;
        --icount)
            # Optional shift argument. Treat a following token as the shift only
            # if it is not another option (a bare --icount means shift=auto).
            if [ -n "${2:-}" ] && [ "${2#-}" = "$2" ]; then
                ICOUNT="$2"; shift 2
            else
                ICOUNT="auto"; shift
            fi
            ;;
        --icount=*) ICOUNT="${1#--icount=}"; shift ;;
        --monitor) MONITOR=1; shift ;;
        --inverse) INVERSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

# Default SD card: if no --sd was given, look for an '<sdcard>_rw' or '<sdcard>'
# directory in the current working directory and use it automatically (the
# '_rw' variant is preferred, so guest changes are written back; otherwise the
# read-only snapshot is used). If neither exists and the terminal is
# interactive, offer to download the Synthstrom factory card contents into
# ./${SD_DIR} and use that.
if [ ${#SD_ARGS[@]} -eq 0 ]; then
    for default_sd in "${SD_DIR}_rw" "${SD_DIR}"; do
        if [ -d "${default_sd}" ]; then
            log "No --sd given; defaulting to ./${default_sd}"
            sd_setup "${default_sd}"
            break
        fi
    done
fi
if [ ${#SD_ARGS[@]} -eq 0 ] && [ -t 0 ]; then
    printf '%s\n' "No SD card specified and no ./${SD_DIR} folder found." >&2
    printf '%s' "Download ${FACTORY_SD_NAME} from Synthstrom and use it? [y/N] " >&2
    read -r reply || reply=""
    case "${reply}" in
        [yY]|[yY][eE][sS])
            download_and_unzip "${FACTORY_SD_URL}" "${SD_DIR}" "${FACTORY_SD_NAME}"
            if [ -d "${SD_DIR}" ]; then
                sd_setup "${SD_DIR}"
            else
                warn "factory card download produced no ./${SD_DIR} folder; continuing without an SD card"
            fi
            ;;
        *)
            log "Continuing without an SD card"
            ;;
    esac
fi

# 'coremidi' bridges a transport to a real host MIDI port via a standalone
# helper (scripts/midi_bridge.c) connected to a QEMU UNIX-socket chardev.
BRIDGE_SPECS=()
DIN_SOCK="${TMPDIR:-/tmp}/delugemu-din.$$.sock"
USB_SOCK="${TMPDIR:-/tmp}/delugemu-usb.$$.sock"
MIDI_BRIDGE_SRC="${REPO_ROOT}/scripts/midi_bridge.c"
MIDI_BRIDGE_BIN="${REPO_ROOT}/build/midi_bridge"

ensure_midi_bridge() {
    case "$(uname -s)" in
        Darwin) ;;
        *) die "'coremidi' MIDI bridging is currently macOS-only" ;;
    esac
    if [ ! -x "${MIDI_BRIDGE_BIN}" ] \
        || [ "${MIDI_BRIDGE_SRC}" -nt "${MIDI_BRIDGE_BIN}" ]; then
        log "Building MIDI bridge helper"
        mkdir -p "$(dirname "${MIDI_BRIDGE_BIN}")"
        cc -O2 -Wall -o "${MIDI_BRIDGE_BIN}" "${MIDI_BRIDGE_SRC}" \
            -framework CoreMIDI -framework CoreFoundation \
            || die "failed to build MIDI bridge helper"
    fi
}

# SCIF0 (DIN MIDI) and the QEMU monitor. The interactive monitor is opt-in
# (--monitor); by default stdio carries only the SCIF0 serial console so a plain
# run does not drop the user at a '(qemu)' prompt. With a MIDI backend SCIF0
# takes that chardev, so the monitor (when enabled) gets its own stdio.
SERIAL_ARGS=()
if [ "${MIDI}" = "coremidi" ]; then
    rm -f "${DIN_SOCK}"
    SERIAL_ARGS=(-chardev "socket,id=din,path=${DIN_SOCK},server=on,wait=off" \
                 -serial chardev:din)
    [ "${MONITOR}" -eq 1 ] && SERIAL_ARGS+=(-monitor stdio) || SERIAL_ARGS+=(-monitor none)
    BRIDGE_SPECS+=("DelugEmu DIN=${DIN_SOCK}")
    log "Bridging SCIF0/DIN-MIDI to a host CoreMIDI port (\"DelugEmu DIN\")"
elif [ -n "${MIDI}" ]; then
    SERIAL_ARGS=(-serial "${MIDI}")
    [ "${MONITOR}" -eq 1 ] && SERIAL_ARGS+=(-monitor stdio) || SERIAL_ARGS+=(-monitor none)
    log "Routing SCIF0/MIDI to chardev: ${MIDI}"
elif [ "${MONITOR}" -eq 1 ] || [ "${DISPLAY_MODE}" = "headless" ]; then
    # Headless (-nographic) naturally muxes the serial console and monitor on
    # stdio; keep that. For a GUI run the monitor is opt-in via --monitor.
    SERIAL_ARGS=(-serial mon:stdio)
else
    SERIAL_ARGS=(-serial stdio)
fi

# Host USB-MIDI device: present the device on USB200 and route its bulk pipes
# to the second serial chardev (serial_hd(1)), which the SoC binds to usb0.
USB_MIDI_ARGS=()
if [ "${USB_MIDI}" = "coremidi" ]; then
    rm -f "${USB_SOCK}"
    USB_MIDI_ARGS=(-chardev "socket,id=usbm,path=${USB_SOCK},server=on,wait=off" \
                   -serial chardev:usbm -global "rza1l-usb.midi=on")
    BRIDGE_SPECS+=("DelugEmu USB=${USB_SOCK}")
    log "Bridging USB-MIDI to a host CoreMIDI port (\"DelugEmu USB\")"
elif [ -n "${USB_MIDI}" ]; then
    USB_MIDI_ARGS=(-serial "${USB_MIDI}" -global "rza1l-usb.midi=on")
    log "Attaching host USB-MIDI device on chardev: ${USB_MIDI}"
fi

# Display mode. On macOS the console window is left non-resizable so Cocoa sizes
# it to the front-panel surface (2256x1584) divided by the display's backing
# scale factor — i.e. a crisp 1:1 window (~1128x792 on a 2x Retina display).
# Enabling zoom-to-fit there makes the window resizable, which makes Cocoa skip
# that surface-sizing path and leave the window stuck at its tiny 640x480 init
# frame; the View > Zoom-to-fit menu item still toggles it on demand. GTK sizes
# correctly with zoom-to-fit, so keep it there.
DISPLAY_ARGS=()
case "${DISPLAY_MODE}" in
    headless) DISPLAY_ARGS=(-nographic) ;;
    console)
        case "$(uname -s)" in
            Darwin)
                DISPLAY_ARGS=(-display cocoa,show-cursor=on)
                ;;
            *)
                DISPLAY_ARGS=(-display gtk,zoom-to-fit=on,show-menubar=off)
                ;;
        esac
        ;;
    none)     DISPLAY_ARGS=(-display none) ;;
    *) die "unknown --display mode '${DISPLAY_MODE}' (console|headless|none)" ;;
esac

# Front-panel skin image. The default is the light "Normal" panel
# (Delugemu_Normal.png) with white unlit pads; --inverse selects the original
# dark panel (Delugemu_Inverse.png) with black unlit pads. The skin device
# loads its image relative to the working directory by default, which breaks
# when run.sh is invoked from another directory (or from a relocatable bundle),
# so resolve it next to the repo/bundle root (REPO_ROOT, set by common.sh) and
# pass an absolute path. DELUGEMU_SKIN overrides the location (and --inverse).
SKIN_ARGS=()
if [ "${INVERSE}" -eq 1 ]; then
    _default_skin="${REPO_ROOT}/Delugemu_Inverse.png"
else
    _default_skin="${REPO_ROOT}/Delugemu_Normal.png"
fi
SKIN_IMAGE="${DELUGEMU_SKIN:-${_default_skin}}"
if [ "${DISPLAY_MODE}" = "console" ]; then
    if [ -f "${SKIN_IMAGE}" ]; then
        SKIN_ARGS=(-global "deluge-skin.image=${SKIN_IMAGE}")
        [ "${INVERSE}" -eq 1 ] && SKIN_ARGS+=(-global "deluge-skin.inverse=on")
    else
        warn "skin image not found at ${SKIN_IMAGE}; the panel will render without its photo background"
    fi
    if [ -n "${SKIN_REFRESH_MS}" ]; then
        case "${SKIN_REFRESH_MS}" in
            *[!0-9]*|'') die "--skin-refresh-ms must be a positive integer (ms)" ;;
        esac
        SKIN_ARGS+=(-global "deluge-skin.refresh-ms=${SKIN_REFRESH_MS}")
    fi
fi

# Front-panel window scale. The native panel (2256x1584) renders 1:1 by default;
# on a HiDPI/Retina display the host shows it at half its pixel count in points,
# a well-sized crisp window. Only when 'auto' is requested (for a low-DPI monitor
# where native would overflow) do we down-sample to whatever fits the primary
# monitor (within ~90% of it); the skin device then down-scales internally and
# opens the host window at the reduced size.
SKIN_IMG_W=2256
SKIN_IMG_H=1584

# Print "<width> <height>" of the primary monitor in pixels, or nothing if it
# cannot be determined. Best-effort and per-OS.
detect_screen_size() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            powershell.exe -NoProfile -Command \
                'Add-Type -AssemblyName System.Windows.Forms; $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds; "$($b.Width) $($b.Height)"' \
                2>/dev/null | tr -d '\r'
            ;;
        Darwin)
            system_profiler SPDisplaysDataType 2>/dev/null \
                | sed -n 's/.*Resolution: \([0-9]*\) x \([0-9]*\).*/\1 \2/p' \
                | head -n 1
            ;;
        Linux)
            if command -v xrandr >/dev/null 2>&1; then
                xrandr 2>/dev/null \
                    | sed -n 's/.*current \([0-9]*\) x \([0-9]*\).*/\1 \2/p' \
                    | head -n 1
            fi
            ;;
    esac
}

# Largest scale (percent, 10..100) at which the panel fits within ~90% of a
# <width> <height> screen. Echoes the integer percent.
compute_fit_scale() {
    local sw="$1" sh="$2"
    local usable_w=$(( sw * 90 / 100 ))
    local usable_h=$(( sh * 90 / 100 ))
    local scale_w=$(( usable_w * 100 / SKIN_IMG_W ))
    local scale_h=$(( usable_h * 100 / SKIN_IMG_H ))
    local scale="${scale_w}"
    [ "${scale_h}" -lt "${scale}" ] && scale="${scale_h}"
    [ "${scale}" -gt 100 ] && scale=100
    [ "${scale}" -lt 10 ] && scale=10
    printf '%s' "${scale}"
}

if [ "${DISPLAY_MODE}" = "console" ]; then
    SKIN_SCALE_PCT=""
    case "${SKIN_SCALE}" in
        ''|auto|fit)
            read -r _scr_w _scr_h < <(detect_screen_size) || true
            if [ -n "${_scr_w:-}" ] && [ -n "${_scr_h:-}" ]; then
                SKIN_SCALE_PCT="$(compute_fit_scale "${_scr_w}" "${_scr_h}")"
                log "Primary monitor ${_scr_w}x${_scr_h}; scaling front panel to ${SKIN_SCALE_PCT}% to fit"
            else
                SKIN_SCALE_PCT="60"
                log "Could not detect monitor size; defaulting front-panel scale to ${SKIN_SCALE_PCT}%"
            fi
            ;;
        native|full|100) SKIN_SCALE_PCT="100" ;;
        *[!0-9]*) die "--skin-scale must be a percent (10-100), 'auto', or 'native'" ;;
        *)
            SKIN_SCALE_PCT="${SKIN_SCALE}"
            [ "${SKIN_SCALE_PCT}" -ge 10 ] && [ "${SKIN_SCALE_PCT}" -le 100 ] \
                || die "--skin-scale percent must be between 10 and 100"
            ;;
    esac
    if [ -n "${SKIN_SCALE_PCT}" ] && [ "${SKIN_SCALE_PCT}" != "100" ]; then
        SKIN_ARGS+=(-global "deluge-skin.scale-percent=${SKIN_SCALE_PCT}")
    fi
fi
# Optional audio backend override, routed to the SSIF (I2S) sink. The device
# opens the OS default backend on its own, so this is only needed to select a
# non-default driver. The SoC builds the SSIF internally, so -global binds the
# audiodev to the device's property. 'auto' resolves to the recommended host
# driver for this OS.
# Resolve the host audio backend (auto-detect when unset) and route the SSIF
# (I2S) output to it. We always pass an explicit -audiodev — rather than letting
# the device open the OS default — so we can enlarge the host playback buffer
# (out.buffer-length). On Windows the dsound voice is pumped from QEMU's main
# loop, the same thread that recomposites the front-panel skin, so a periodic
# full-frame skin upload can briefly stall the audio service; a generous host
# buffer lets the OS audio DMA ride those stalls without an audible gap. 'auto'
# (or unset) resolves to the recommended host driver for this OS; 'none' wires a
# silent sink.
AUDIO_ARGS=()
if [ "${AUDIO}" = "none" ]; then
    AUDIO_ARGS=(-audiodev "none,id=deluge0"
                -global "rza1l-ssif.audiodev=deluge0")
    log "Routing SSIF audio to backend: none"
else
    if [ -z "${AUDIO}" ] || [ "${AUDIO}" = "auto" ]; then
        case "$(uname -s)" in
            Darwin)  AUDIO="coreaudio" ;;
            Linux)   AUDIO="pa" ;;
            MINGW*|MSYS*|CYGWIN*) AUDIO="dsound" ;;
            *)       AUDIO="sdl" ;;
        esac
        log "Audio backend auto-selected for $(uname -s): ${AUDIO}"
    fi
    AUDIO_ARGS=(-audiodev "${AUDIO},id=deluge0,out.buffer-length=${AUDIO_HOST_BUFFER_US}"
                -global "rza1l-ssif.audiodev=deluge0")
    log "Routing SSIF audio to backend: ${AUDIO} (host buffer ${AUDIO_HOST_BUFFER_US} us)"
fi

# Optional output buffer cushion override (milliseconds), bound to the SSIF's
# prime-ms property. Lower = less perceived delay when playing the emulator
# live; raise it if you hear dropouts.
if [ -n "${AUDIO_BUFFER}" ]; then
    case "${AUDIO_BUFFER}" in
        ''|*[!0-9]*) die "--audio-buffer must be a non-negative integer (ms)" ;;
    esac
    AUDIO_ARGS+=(-global "rza1l-ssif.prime-ms=${AUDIO_BUFFER}")
    log "SSIF output buffer cushion: ${AUDIO_BUFFER} ms"
fi

# Optional render-head clamp (advanced), bound to the SSIF's tx-render-head
# property. When set, the ring sampler bounds its reads by the firmware's render
# head at this guest address instead of the wall-clock DMA play head, removing
# the ring-lap distortion under heavy load. The address is firmware-build
# specific, so this is opt-in; unset leaves the firmware-independent play-head
# behaviour. Accepts hex (0x...), decimal, or 'auto' to have the SSIF find the
# render head itself at runtime (useful for stripped firmware with no symbols).
if [ -n "${TX_RENDER_HEAD}" ]; then
    case "${TX_RENDER_HEAD}" in
        auto)
            AUDIO_ARGS+=(-global "rza1l-ssif.tx-render-head-auto=on")
            log "SSIF audio bounded by auto-detected firmware render head" ;;
        0x[0-9A-Fa-f]*|[0-9]*)
            AUDIO_ARGS+=(-global "rza1l-ssif.tx-render-head=${TX_RENDER_HEAD}")
            log "SSIF audio bounded by firmware render head at: ${TX_RENDER_HEAD}" ;;
        *) die "--tx-render-head must be a guest address (hex 0x..., decimal) or 'auto'" ;;
    esac
fi

# Optional deterministic instruction-counted clock (-icount). Locks the guest to
# a virtual clock paced to real time: audio stays internally consistent (no
# stale-ring distortion or dropouts) at the cost of capping the guest to <= real
# time, so heavy DSP load slows playback instead of breaking it up. Off by
# default. 'auto' lets QEMU tune the shift; otherwise it must be an integer
# power-of-two ns-per-instruction exponent.
ICOUNT_ARGS=()
if [ -n "${ICOUNT}" ]; then
    case "${ICOUNT}" in
        auto|[0-9]|[0-9][0-9]) : ;;
        *) die "--icount shift must be 'auto' or an integer (e.g. 0-20)" ;;
    esac
    ICOUNT_ARGS=(-icount "shift=${ICOUNT},sleep=on")
    log "Deterministic icount clock enabled: shift=${ICOUNT} (guest capped to real time)"
fi

[ ${#SD_ARGS[@]} -gt 0 ] && log "Attaching SD image"

# Post-run cleanup: reap the MIDI bridge and its sockets, write back a '_rw'
# folder-backed SD, and remove the temporary SD image. Idempotent: clears its
# own traps so it runs at most once across EXIT/INT/TERM.
cleanup() {
    trap - EXIT INT TERM
    if [ -n "${BRIDGE_PID}" ]; then
        kill "${BRIDGE_PID}" 2>/dev/null || true
        rm -f "${DIN_SOCK}" "${USB_SOCK}"
    fi
    if [ -n "${SD_TMP_IMG}" ]; then
        if [ "${SD_WRITEBACK}" = "1" ]; then
            sd_writeback || true
        fi
        rm -f "${SD_TMP_IMG}"
    fi
}

# Launch the host MIDI bridge for any 'coremidi' transports. It connects to the
# QEMU sockets once they appear, so it can start before QEMU.
BRIDGE_PID=""
if [ ${#BRIDGE_SPECS[@]} -gt 0 ]; then
    ensure_midi_bridge
    "${MIDI_BRIDGE_BIN}" "${BRIDGE_SPECS[@]}" &
    BRIDGE_PID=$!
fi

log "Launching ${DELUGE_MACHINE} machine with ${FIRMWARE} (display=${DISPLAY_MODE})"
QEMU_CMD=("${BIN}"
    -M "${DELUGE_MACHINE}"
    -kernel "${FIRMWARE}"
    "${SERIAL_ARGS[@]}"
    "${USB_MIDI_ARGS[@]+"${USB_MIDI_ARGS[@]}"}"
    "${DISPLAY_ARGS[@]+"${DISPLAY_ARGS[@]}"}"
    "${SKIN_ARGS[@]+"${SKIN_ARGS[@]}"}"
    "${AUDIO_ARGS[@]+"${AUDIO_ARGS[@]}"}"
    "${ICOUNT_ARGS[@]+"${ICOUNT_ARGS[@]}"}"
    "${SD_ARGS[@]+"${SD_ARGS[@]}"}"
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}")

if [ -n "${BRIDGE_PID}" ] || [ -n "${SD_TMP_IMG}" ]; then
    # Keep this shell alive so the cleanup trap can reap the bridge and/or write
    # back and remove the temporary SD image.
    trap cleanup EXIT INT TERM
    "${QEMU_CMD[@]}"
else
    exec "${QEMU_CMD[@]}"
fi

