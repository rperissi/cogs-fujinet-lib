#!/usr/bin/env bash
#
# mkdisk.sh - wrap a cc65 Apple II executable (AppleSingle .a2s, as the
# fujinet-apps MekkoGX build emits) in a bootable 140K ProDOS 8 disk.
#
#   tools/mkdisk.sh <NAME> <prog.a2s> <out.po> [prodos-base.po]
#
# NAME is the ProDOS file name (<= 15 chars, e.g. FCS). The disk gets
# PRODOS, NAME.SYSTEM (cc65's loader.system, which launches NAME) and
# NAME itself. The base image supplies the PRODOS kernel and the boot
# block; default is the ProDOS 2.4.3 release MekkoGX caches.
#
# Needs AppleCommander.jar (AC_JAR or a few known spots) and Java.

set -euo pipefail
NAME="$1"; PROG="$2"; OUT="$3"; BASE="${4:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WS="$(cd "$REPO/.." && pwd)"

if [ -z "$BASE" ]; then
    for c in "$REPO/_cache/PRODOS8-2.4.3.po" "$WS/reference/fujinet-5cardstud/_cache/apple2/PRODOS8-2.4.3.po"; do
        [ -f "$c" ] && BASE="$c" && break
    done
fi
if [ -z "$BASE" ]; then
    mkdir -p "$REPO/_cache"
    BASE="$REPO/_cache/PRODOS8-2.4.3.po"
    curl -sL -o "$BASE" https://releases.prodos8.com/ProDOS_2_4_3.po
fi
[ -f "$BASE" ] || { echo "error: no ProDOS base image" >&2; exit 1; }

AC_JAR="${AC_JAR:-}"
if [ -z "$AC_JAR" ]; then
    for c in "$WS/AppleCommander.jar" "$REPO/AppleCommander.jar" "$HOME/AppleCommander.jar"; do
        [ -f "$c" ] && AC_JAR="$c" && break
    done
fi
[ -f "${AC_JAR:-/nonexistent}" ] || { echo "error: AppleCommander.jar not found (set AC_JAR)" >&2; exit 1; }
JAVA=""
for c in "/opt/homebrew/opt/openjdk/bin/java" "/usr/local/opt/openjdk/bin/java" "java"; do
    if "$c" -version >/dev/null 2>&1; then JAVA="$c"; break; fi
done
[ -n "$JAVA" ] || { echo "error: no Java runtime" >&2; exit 1; }
ac() { "$JAVA" -jar "$AC_JAR" "$@"; }

LOADER="$(cl65 --print-target-path --target apple2)/apple2/util/loader.system"
[ -f "$LOADER" ] || { echo "error: cc65 loader.system not found at $LOADER" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ac -g "$BASE" PRODOS "$TMP/PRODOS.bin"
[ -s "$TMP/PRODOS.bin" ] || { echo "error: base has no PRODOS file" >&2; exit 1; }

rm -f "$OUT"
ac -pro140 "$OUT" "$NAME" >/dev/null
ac -p  "$OUT" PRODOS SYS 0x2000 < "$TMP/PRODOS.bin"
ac -p  "$OUT" "$NAME.SYSTEM" SYS 0x2000 < "$LOADER"
ac -as "$OUT" "$NAME" < "$PROG"
dd if="$BASE" of="$OUT" bs=512 count=2 conv=notrunc status=none

echo "==> $OUT"
ac -l "$OUT"
