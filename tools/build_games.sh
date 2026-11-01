#!/usr/bin/env bash
#
# build_games.sh - build the FujiNet lobby games for Apple II against this
# library and wrap each in a bootable ProDOS disk under dist/.
#
#   tools/build_games.sh [games-dir]
#
# games-dir holds clones of the FujiNetWIFI game repos (default: the
# reference/ folder beside this checkout). Missing clones are fetched
# shallow. Needs cc65, make, python3 (MekkoGX), Java + AppleCommander.jar.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
GAMES="${1:-$(cd "$REPO/.." && pwd)/reference}"
mkdir -p "$REPO/dist" "$GAMES"

make -C "$REPO" >/dev/null

# repo  prog  ProDOS-name  disk
SET=(
  "fujinet-5cardstud   fcs      FCS      5card-cogs.po"
  "fujinet-texasHoldEm texas    TEXAS    texas-cogs.po"
  "fujinet-fujitzee    fujitzee FUJITZEE fujitzee-cogs.po"
  "fujinet-battleship  fbs      FBS      battleship-cogs.po"
)

for row in "${SET[@]}"; do
    read -r repo prog pname disk <<<"$row"
    dir="$GAMES/$repo"
    if [ ! -d "$dir" ]; then
        git clone --depth 1 "https://github.com/FujiNetWIFI/$repo" "$dir"
    fi
    echo "### $repo"
    # MekkoGX errors at the disk step when acx is absent; the .a2s is done by then.
    (cd "$dir" && make clean >/dev/null 2>&1; make apple2 FUJINET_LIB="$REPO" >/dev/null 2>&1) || true
    a2s="$dir/r2r/apple2/$prog.a2s"
    [ -f "$a2s" ] || { echo "error: $a2s not produced" >&2; exit 1; }
    "$HERE/mkdisk.sh" "$pname" "$a2s" "$REPO/dist/$disk" >/dev/null
    echo "    dist/$disk"
done
