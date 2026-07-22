#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Audio-throughput benchmark (roadmap M6).
#
# Drives the firmware through a fixed, worst-case synthesis load (load FAM1 — a
# 127-sound song — and press PLAY) while the SSIF production-rate probe
# (DELUGEMU_SSIF_STATS) records, per virtual second, how much freshly rendered
# audio reaches the staging FIFO against the 352,800 B/s real-time target and how
# often the primed output voice underran. Reports those figures plus the host
# wall-clock the run took, so a change to the emulator (or firmware) can be A/B'd
# for an audio-throughput regression.
#
# What the numbers mean:
#   * production %  — freshly rendered guest audio as a fraction of real time.
#                     On a host fast enough to render in real time this pins near
#                     100%; it drops (and underruns climb) only when the emulated
#                     CPU cannot keep up. So the regression signal is sharpest on
#                     a throttled/slow host (see --throttle below); on a fast dev
#                     machine the benchmark mainly confirms "still ~100%, no new
#                     underruns", which is the no-regression gate.
#   * host elapsed  — wall-clock for the fixed key sequence. Lower is faster.
#
# Determinism: pass --icount <shift> for an instruction-counted virtual clock.
# Note the SSIF probe counts in virtual time, so under --icount production pins
# ~100% by construction (the "wall-clock delivery gate" caveat in
# docs/firmware-perf.md); use --icount for a deterministic functional run and the
# default real-time mode (ideally throttled) for the throughput signal.
#
# Firmware is not shipped with the repo. The benchmark locates an image and SKIPS
# cleanly (exit 0) when firmware, an SD image, or python3 is missing, so it is
# safe to wire into CI as a smoke check.
#
# Usage:
#   ./tests/audio-bench.sh                         # auto-locate assets
#   ./tests/audio-bench.sh --repeat 5              # average over 5 runs
#   ./tests/audio-bench.sh --icount 2              # deterministic clock
#   ./tests/audio-bench.sh --throttle "taskpolicy -b"  # run qemu throttled
#   DELUGE_FIRMWARE=path DELUGE_SD_IMG=img ./tests/audio-bench.sh
#   DELUGEMU_BENCH_RENDER_HEAD=0x20031f2c ./tests/audio-bench.sh
#   ./tests/audio-bench.sh --gate --min-production 90 --max-underruns 0
#
# Environment overrides:
#   DELUGE_FIRMWARE            firmware .elf/.bin (else auto-located)
#   DELUGE_SD_IMG             SD image (else build/deluge_sd.img, built if absent)
#   DELUGEMU_BENCH_RENDER_HEAD render-head guest address (else derived / 'auto')
#   DELUGEMU_BENCH_SEQ         dz_play.py key sequence (else the FAM1 default)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) EXE_SUFFIX=".exe" ;;
    *)                    EXE_SUFFIX="" ;;
esac
BIN="${REPO_ROOT}/qemu/build/qemu-system-arm${EXE_SUFFIX}"

fail() { printf 'BENCH FAIL: %s\n' "$*" >&2; exit 1; }
skip() { printf 'BENCH SKIP: %s\n' "$*"; exit 0; }

# --- Options ----------------------------------------------------------------

REPEAT=3
ICOUNT=""
THROTTLE=""
GATE=0
MIN_PRODUCTION=0        # gate floor: min mean production % (0 = don't gate)
MAX_UNDERRUNS=-1        # gate ceiling: total underruns (-1 = don't gate)
# Canonical worst-case load: boot 6 s, LOAD (l), SELECT-click (ret) loads the
# highlighted FAM1, PLAY (spc), let it render, then quit. Matches the profiling
# sequence in docs/firmware-perf.md.
SEQ="${DELUGEMU_BENCH_SEQ:-6.0:_wait 0:l:tap 3.0:ret:tap 8.0:spc:tap 14.0:_quit}"

while [ $# -gt 0 ]; do
    case "$1" in
        --repeat)  [ -n "${2:-}" ] || fail "--repeat needs a count"; REPEAT="$2"; shift 2 ;;
        --icount)
            if [ -n "${2:-}" ] && [ "${2#-}" = "$2" ]; then ICOUNT="$2"; shift 2
            else ICOUNT="auto"; shift; fi ;;
        --throttle) [ -n "${2:-}" ] || fail "--throttle needs a command prefix"; THROTTLE="$2"; shift 2 ;;
        --gate)    GATE=1; shift ;;
        --min-production) [ -n "${2:-}" ] || fail "--min-production needs a percent"; MIN_PRODUCTION="$2"; shift 2 ;;
        --max-underruns)  [ -n "${2:-}" ] || fail "--max-underruns needs a count"; MAX_UNDERRUNS="$2"; shift 2 ;;
        --seq)     [ -n "${2:-}" ] || fail "--seq needs a sequence"; SEQ="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,50p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

# --- Preconditions ----------------------------------------------------------

[ -x "${BIN}" ] || fail "qemu-system-arm not built at ${BIN} (run scripts/build.sh)"
command -v python3 >/dev/null 2>&1 || skip "python3 not found (needed to drive QMP)"

# Locate a firmware image: explicit override, then the usual build outputs.
FIRMWARE="${DELUGE_FIRMWARE:-}"
if [ -z "${FIRMWARE}" ]; then
    for cand in "${REPO_ROOT}/firmware/deluge.elf" "${REPO_ROOT}/firmware/deluge.bin"; do
        if [ -f "${cand}" ]; then FIRMWARE="${cand}"; break; fi
    done
fi
[ -n "${FIRMWARE}" ] && [ -f "${FIRMWARE}" ] \
    || skip "no firmware image found (set DELUGE_FIRMWARE=path to run)"

# An SD image is required — FAM1 loads from the card. Build one from ./sdcard if
# the default image is absent and the content folder exists.
SD_IMG="${DELUGE_SD_IMG:-${REPO_ROOT}/build/deluge_sd.img}"
if [ ! -f "${SD_IMG}" ]; then
    if [ -d "${REPO_ROOT}/sdcard" ] && [ -x "${REPO_ROOT}/scripts/mksd.sh" ]; then
        printf '  building SD image from ./sdcard...\n'
        "${REPO_ROOT}/scripts/mksd.sh" "${REPO_ROOT}/sdcard" "${SD_IMG}" >/dev/null 2>&1 \
            || skip "could not build SD image (need sdcard/ content + mksd tooling)"
    else
        skip "no SD image at ${SD_IMG} and no ./sdcard to build one from"
    fi
fi

# Resolve the render head: explicit env, else derive from an unstripped ELF's
# symbol table, else let the emulator auto-detect (run.sh's default).
RENDER_HEAD="${DELUGEMU_BENCH_RENDER_HEAD:-}"
if [ -z "${RENDER_HEAD}" ] && [ "${FIRMWARE##*.}" = "elf" ] \
        && command -v arm-none-eabi-nm >/dev/null 2>&1; then
    # awk exits at the first match, so nm sees SIGPIPE; swallow it (pipefail).
    RENDER_HEAD="$(arm-none-eabi-nm "${FIRMWARE}" 2>/dev/null \
        | awk '/i2sTXBufferPos/ {print "0x"$1; exit}' || true)"
fi
[ -n "${RENDER_HEAD}" ] || RENDER_HEAD="auto"

printf 'BENCH: firmware=%s sd=%s render-head=%s icount=%s repeat=%s\n' \
    "${FIRMWARE#${REPO_ROOT}/}" "${SD_IMG#${REPO_ROOT}/}" \
    "${RENDER_HEAD}" "${ICOUNT:-off}" "${REPEAT}"

# --- One run ----------------------------------------------------------------

# Echo "<host_ms> <mean_prod_pct> <min_prod_pct> <total_underruns> <samples>"
# for a single benchmark run, or "ERR" on failure.
run_once() {
    local idx="$1"
    local sock stats log qemu_pid rc start end
    sock="$(mktemp -u "${TMPDIR:-/tmp}/dz_qmp.XXXXXX.sock")"
    stats="$(mktemp "${TMPDIR:-/tmp}/dz_stats.XXXXXX")"
    log="$(mktemp "${TMPDIR:-/tmp}/dz_run.XXXXXX")"
    rm -f "${sock}"

    local run_args=(
        "${FIRMWARE}" --display none --audio none
        --tx-render-head "${RENDER_HEAD}" --sd "${SD_IMG}"
    )
    [ -n "${ICOUNT}" ] && run_args+=(--icount "${ICOUNT}")
    run_args+=(-- -qmp "unix:${sock},server,nowait")

    start="$(python3 -c 'import time;print(int(time.monotonic()*1000))')"
    # shellcheck disable=SC2086
    DELUGEMU_SSIF_STATS="${stats}" ${THROTTLE} \
        "${REPO_ROOT}/scripts/run.sh" "${run_args[@]}" </dev/null >"${log}" 2>&1 &
    qemu_pid=$!

    # Drive the load. dz_play.py connects when QMP is up and quits at the end.
    if ! python3 "${REPO_ROOT}/scripts/dz_play.py" "${sock}" "${SEQ}" \
            >>"${log}" 2>&1; then
        kill "${qemu_pid}" 2>/dev/null || true
    fi
    wait "${qemu_pid}" 2>/dev/null || true
    rc=$?
    end="$(python3 -c 'import time;print(int(time.monotonic()*1000))')"

    # Parse the stats file: lines like
    #   ssif-stats: production 352800 B/s (100.0% of 352800), underruns 0/s, fifo 15.0 ms
    local parsed
    parsed="$(awk '
        /ssif-stats:/ {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^\([0-9.]+%$/) { p = $i; gsub(/[(%]/, "", p);
                                           sum += p; n++;
                                           if (mn == "" || p < mn) mn = p }
                if ($i == "underruns") { u = $(i+1); sub(/\/s.*/, "", u); ur += u }
            }
        }
        END {
            if (n == 0) { print "NODATA"; exit }
            printf "%.1f %.1f %d %d", sum / n, mn, ur, n
        }' "${stats}")"

    rm -f "${sock}" "${stats}" "${log}"

    if [ "${parsed}" = "NODATA" ] || [ -z "${parsed}" ]; then
        printf 'ERR'
        return
    fi
    printf '%s %s' "$((end - start))" "${parsed}"
}

# --- Drive the repeats and aggregate ----------------------------------------

sum_ms=0; sum_prod=0; min_prod=100000; sum_ur=0; ok=0
for i in $(seq 1 "${REPEAT}"); do
    read -r ms mean_prod min_p ur samples <<<"$(run_once "${i}")" || true
    if [ "${ms:-ERR}" = "ERR" ] || [ -z "${ms:-}" ]; then
        printf '  run %s/%s: no audio stats captured (playback may not have started)\n' \
            "${i}" "${REPEAT}"
        continue
    fi
    printf '  run %s/%s: host %sms, production mean %.1f%% min %.1f%%, underruns %s (%s samples)\n' \
        "${i}" "${REPEAT}" "${ms}" "${mean_prod}" "${min_p}" "${ur}" "${samples}"
    sum_ms=$((sum_ms + ms)); sum_ur=$((sum_ur + ur)); ok=$((ok + 1))
    sum_prod="$(python3 -c "print(${sum_prod} + ${mean_prod})")"
    min_prod="$(python3 -c "print(min(${min_prod}, ${min_p}))")"
done

[ "${ok}" -gt 0 ] || fail "no run produced audio stats (is playback reaching the SSIF?)"

mean_ms=$((sum_ms / ok))
mean_prod="$(python3 -c "print(round(${sum_prod} / ${ok}, 1))")"
printf 'BENCH RESULT: runs=%s host-mean=%sms production-mean=%s%% production-min=%s%% underruns-total=%s\n' \
    "${ok}" "${mean_ms}" "${mean_prod}" "${min_prod}" "${sum_ur}"

# --- Optional gate ----------------------------------------------------------

if [ "${GATE}" -eq 1 ]; then
    rc=0
    if python3 -c "import sys; sys.exit(0 if ${mean_prod} >= ${MIN_PRODUCTION} else 1)"; then :; else
        printf 'BENCH GATE: production mean %s%% below floor %s%%\n' "${mean_prod}" "${MIN_PRODUCTION}" >&2
        rc=1
    fi
    if [ "${MAX_UNDERRUNS}" -ge 0 ] && [ "${sum_ur}" -gt "${MAX_UNDERRUNS}" ]; then
        printf 'BENCH GATE: total underruns %s exceed ceiling %s\n' "${sum_ur}" "${MAX_UNDERRUNS}" >&2
        rc=1
    fi
    [ "${rc}" -eq 0 ] || exit 1
    printf 'BENCH GATE: pass\n'
fi

printf 'BENCH OK\n'
