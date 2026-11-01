#!/usr/bin/env bash
#
# run_app.sh - boot a FujiNet app disk on an emulated Apple //e with the
# CoGS MAME card, headless, screenshots + the card's HTTP log.
#
#   tools/mame/run_app.sh <disk.po> [seconds] [FN_KEYS schedule]
#
# Env: COGS_BIN (default ../mame-cogs/cogs), ROMP (Ample roms), MACH
# (apple2ee), SLOT (4), OUTDIR (/tmp/fnapp-<name>), THROTTLE=0 to run flat out.

set -euo pipefail
DISK="$1"; SECS="${2:-120}"; KEYS="${3:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WS="$(cd "$REPO/.." && pwd)"
COGS_BIN="${COGS_BIN:-$WS/mame-cogs/cogs}"
ROMP="${ROMP:-$HOME/Library/Application Support/Ample/roms}"
MACH="${MACH:-apple2ee}"
SLOT="${SLOT:-4}"
NAME="$(basename "${DISK%.*}")"
OUTDIR="${OUTDIR:-/tmp/fnapp-$NAME}"
rm -rf "$OUTDIR"; mkdir -p "$OUTDIR/snap" "$OUTDIR/nv"
DISK="$(cd "$(dirname "$DISK")" && pwd)/$(basename "$DISK")"
# Seed the emulated card's NV with saved Wi-Fi creds so it auto-joins at
# power-up, the way a configured card does on iron ("CGN1" + ssid + psk +
# empty prefs + empty boot URL; see cogs_backend.c nv_save).
printf 'CGN1\x07CoGSNet\x08password\x00\x00' > "$OUTDIR/nv/cogs.nv"
THR=(-skip_gameinfo)
[ "${THROTTLE:-1}" = "0" ] && THR+=(-nothrottle)

cd "$(dirname "$COGS_BIN")"
FN_KEYS="$KEYS" FN_SNAP="${FN_SNAP:-10}" COGS_EMU_NVRAM="$OUTDIR/nv/cogs.nv" \
timeout $((SECS + 45)) "./$(basename "$COGS_BIN")" "$MACH" -rompath "$ROMP" \
	-sl"$SLOT" cogs -sl6 diskiing -flop1 "$DISK" \
	-video none -sound none "${THR[@]}" \
	-nvram_directory "$OUTDIR/nv" -snapshot_directory "$OUTDIR/snap" -snapname "%g_%i" \
	-autoboot_script "$HERE/fnapp_probe.lua" -seconds_to_run "$SECS" 2>&1 | tee "$OUTDIR/mame.log" | \
	grep -E 'cogs: http|FNAPP|cogs: .*(err|FAIL|DROP)' || true
echo "==> snapshots in $OUTDIR/snap, log $OUTDIR/mame.log"
ls "$OUTDIR/snap" | tail -3
