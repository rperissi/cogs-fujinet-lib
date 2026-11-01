# cogs-fujinet-lib

[fujinet-lib](https://github.com/FujiNetWIFI/fujinet-lib) with a CoGS card
bus underneath. Apple II programs written against the fujinet-lib network
API (`network_open`, `network_read`, `network_json_query`, appkeys, ...)
link against this library unmodified and run their HTTP and JSON traffic
through a CoGS card in the slot instead of a FujiNet on the SmartPort.

It exists so the FujiNet community's Apple II lobby games (5 Card Stud,
Texas Hold'em, Fujitzee, Battleship, Fujirkle) can be played from a CoGS
card. The game servers are Eric Carr's; the game clients are unchanged.
Only the bus layer is new.

Licence: GPL-3.0, same as upstream fujinet-lib. The CoGS card driver in
`src/cogs_card.c` was written from the published card protocol
(cogs-gameroom `docs/card-protocol.md`, MIT) and carries no code from the
card firmware or the PolyForm `cogslib`.

## Layout

- `fujinet-lib/` submodule, upstream fujinet-lib. `common/` and `include/`
  are used as-is.
- `src/cogs_card.[ch]` card driver: slot scan, framing, reply wait.
- `src/fujinet-bus-cogs.c` the bus: `fuji_bus_call`, `network_bus_read`,
  `network_bus_write`, `fuji_bus_appkey_read/write`.
- `src/appkey_disk.c` + `src/mli.s` appkeys as files on the game's ProDOS
  volume (`AK<creator><app><key>`, type $F1). Read-only volume: the write
  fails quietly and the game asks again next boot.
- `src/compat4.c` shims for apps that still ship fujinet-lib 4.x headers.
- `tools/mkdisk.sh` wraps a cc65 executable in a bootable 140K ProDOS disk.
- `tools/mame/run_app.sh` boots a disk headless on an emulated //e with the
  CoGS MAME card and logs the card's HTTP traffic.

## Build the library

Needs cc65 (`cl65`, `ar65`) on the path.

    git submodule update --init
    make

Output is `build/fujinet-apple2-<ver>.lib` with the headers copied beside
it, which is the layout MekkoGX and `fujinet-lib.mk` expect.

Tunables (pass as `EXTRA_CFLAGS=-DNAME=value`):

| define | default | what |
|---|---|---|
| `FN_COGS_UNITS` | 1 | network units (N1:, N2:, ...) |
| `FN_COGS_BUF` | 768 | per-unit body buffer, bytes |
| `FN_COGS_URL` | 128 | longest URL |
| `FN_COGS_HDRS` | 0 | request header buffer; 0 turns header collection off |
| `FN_COGS_APPKEY` | 64 | appkey value size |
| `FN_COGS_APPKEY_NODISK` | unset | keep appkeys in RAM only (no ProDOS files) |
| `COGS_SLOT` | scan | fix the slot instead of scanning 7..1 |
| `FN_COGS_NO_LC` | unset | keep the driver out of the language card segment |

The card driver and the less hot bus paths go in the `LC` segment
(language card bank 2, $D400) so the game keeps its main RAM. Every
MekkoGX Apple II config already declares that segment. The entry points
re-enable bank 2 for reading on the way in and put the ROM back on the
way out if that is how they found it, because some apps switch the ROM in
for an ID byte and never switch back (Battleship's `identify.s`), which
would otherwise make the LC code disappear under the library.

## Build a game against it

    git clone https://github.com/FujiNetWIFI/fujinet-5cardstud
    cd fujinet-5cardstud
    make apple2 FUJINET_LIB=/abs/path/to/cogs-fujinet-lib

MekkoGX stops at the disk step if `acx` is missing; the executable is
already in `r2r/apple2/<prog>.a2s` by then. Then:

    /abs/path/to/cogs-fujinet-lib/tools/mkdisk.sh FCS r2r/apple2/fcs.a2s fcs-cogs.po

`tools/build_games.sh` does the five lobby games in one go into `dist/`.

## Test headless

    tools/mame/run_app.sh dist/5card-cogs.po 120 "25:COGSTEST\r;55:\r"

Seeds the emulated card with Wi-Fi credentials, boots the disk, types the
scheduled keys, snapshots every `FN_SNAP` seconds into `/tmp/fnapp-<name>/snap`
and prints each HTTP transaction the card made. `FN_PC=1` adds the 6502
PC to each snapshot line, which is how the Battleship LC problem was found.

Key schedules that reach a table on the live servers (emulated //e, throttled):

| disk | keys |
|---|---|
| 5card, texas | `25:COGSTEST\r;55:\r;90:\r` |
| fujitzee, fujirkle | `25:COGSTEST\r;55:\r;75:\r;95:\r;110:\r` |
| battleship | `40:COGSTEST\r;60:\r;85:\r;115:\r` (title takes ~35 s, then help) |

All five were seen seated at a table and polling state on 2026-11-01.

## What is and is not covered

Works: HTTP GET/POST/PUT/DELETE over http and https, lazy fetch on first
read, JSON parse and pointer queries (translated to the card's path syntax,
scalar `TRUE`/`FALSE`/`NULL` spelled the FujiNet way), STATUS with avail
and end-of-file, `GET_WIFISTATUS`, `RANDOM_NUMBER`, appkeys.

Not covered yet, in rough order of interest:

- Appkeys are kept in RAM, so a player name is asked again at every boot.
  The card has no key store today.
- `tcp://` and `udp://` devicespecs (Syn-Ack Wire, Royal Game of Ur).
- Request headers are compiled out unless `FN_COGS_HDRS` is set.
- A JSON query on an object or array returns the raw JSON text, where
  FujiNet flattens it to key/value lines.
- `GET_ADAPTERCONFIG`, `GET_TIME`, `GET_SSID`, SD card and disk commands.
