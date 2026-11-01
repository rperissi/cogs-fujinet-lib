/*
 * cogs_card.h - minimal CoGS card driver for the fujinet-lib CoGS bus.
 *
 * Copyright (C) 2026 Rob Perissi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. See LICENSE.
 *
 * Written against the published CoGS card protocol (spec v0.7, the
 * "lobby client" subset in cogs-gameroom/docs/card-protocol.md). The card
 * is a slot device: a byte FIFO at DATA plus a STATUS register, carrying
 * frames of op(1) len(u16 LE) payload[len]. The host asks for a whole
 * HTTPS transaction in one frame and gets the body back as events; JSON
 * fields come back one value at a time via JSON_GET. The 6502 never sees
 * TCP, TLS or JSON text.
 */
#ifndef COGS_CARD_H
#define COGS_CARD_H

#include <stdint.h>
#include <stdbool.h>

/* Commands */
#define COGS_CMD_STATUS     0x01
#define COGS_CMD_WIFI_JOIN  0x03
#define COGS_CMD_TIME       0x05
#define COGS_CMD_NETINFO    0x08
#define COGS_CMD_HTTP       0x20
#define COGS_CMD_RANDOM     0x31
#define COGS_CMD_JSON_GET   0x32

/* Replies and events */
#define COGS_RSP(op)        ((op) | 0x80)
#define COGS_EVT_DATA       0x91
#define COGS_EVT_CLOSED     0x92
#define COGS_EVT_ERROR      0x93
#define COGS_EVT_WIFI_STATE 0x94
#define COGS_EVT_HTTP_DONE  0x95

/* HTTP flags */
#define COGS_HTTP_RETAIN    0x04
#define COGS_HTTP_KEEPALIVE 0x08

/* STATUS register bits */
#define COGS_ST_RX_AVAIL    0x80
#define COGS_ST_TX_READY    0x40
#define COGS_ST_LINK_UP     0x10
#define COGS_ST_ERROR       0x08

/* Card error codes ($93 ERROR payload code byte, JSON_GET result byte) */
#define COGS_E_OK            0
#define COGS_E_NOT_JOINED    1
#define COGS_E_DNS_FAIL      2
#define COGS_E_CONN_TIMEOUT  3
#define COGS_E_TLS_HANDSHAKE 5
#define COGS_E_CERT_VERIFY   6
#define COGS_E_OVERRUN       9
#define COGS_E_UNSUPPORTED   11
#define COGS_E_NO_RETAINED   13
#define COGS_E_JSON_NOPATH   14

/* Transport results */
#define COGS_OK          0
#define COGS_ERR_NOCARD  (-1)
#define COGS_ERR_TIMEOUT (-2)
#define COGS_ERR_PROTO   (-3)

/* Find the card (slots 1..7, or COGS_SLOT if the app defines it) and
 * remember it. Returns the slot, 0 if none. Safe to call repeatedly. */
uint8_t cogs_card_find(void);

/* Bring the link up: WIFI_JOIN with the card's stored credentials when
 * STATUS bit 4 is clear, then wait for the card clock (TIME != 0) so TLS
 * can verify certificates. Returns COGS_OK or a COGS_ERR_*. */
int8_t cogs_card_online(void);

/* Frame output. begin writes the header, bytes/byte append payload. The
 * caller is responsible for the total matching len. */
void cogs_tx_begin(uint8_t op, uint16_t len);
void cogs_tx_bytes(const void *p, uint16_t len);
void cogs_tx_byte(uint8_t b);
void cogs_tx_u16(uint16_t v);

/* Frame input. rx_header blocks until a header arrives (long backstop
 * timeout, the card times out its own work first); returns COGS_OK and
 * fills op/len, or COGS_ERR_TIMEOUT. Then read exactly len bytes with
 * rx_byte / rx_bytes / rx_skip. */
int8_t  cogs_rx_header(uint8_t *op, uint16_t *len);
uint8_t cogs_rx_byte(void);
void    cogs_rx_bytes(void *p, uint16_t len);
void    cogs_rx_skip(uint16_t len);

/* Round trip for small fixed replies: send a frame already queued with
 * cogs_tx_*, then wait for reply opcode 'want' (discarding events),
 * copying up to cap payload bytes into out. *got is the payload length.
 * A $93 ERROR for socket $FF while waiting ends the wait with
 * COGS_ERR_PROTO and the card code in *errcode (may be NULL). */
int8_t cogs_wait_reply(uint8_t want, void *out, uint16_t cap,
                       uint16_t *got, uint8_t *errcode);

/* Slot base the driver settled on (0xC080 + slot*16), 0 if none. */
extern uint16_t cogs_base;
extern uint8_t  cogs_slot;

/* Language card guard. The driver and the colder bus paths live in the LC
 * segment (bank 2, $D400). cc65's crt0 leaves bank 2 read-enabled, but
 * some apps switch the ROM in for an ID byte and never switch back
 * (Battleship's identify.s does `bit $C082`), which makes the LC code
 * vanish under us. Every entry point an app or fujinet-lib common code
 * can reach from main RAM wraps its LC calls in these: ENTER re-enables
 * bank 2 for reading, LEAVE puts the ROM back if that is how we found
 * it. Read-only soft switches, no write enable, so ProDOS and the app's
 * own LC use are untouched. Nests. No-ops when the LC segment is not
 * used (FN_COGS_NO_LC). */
#ifndef FN_COGS_NO_LC
extern uint8_t cogs_lc_depth, cogs_lc_was_rom;
/* The switches are poked with inline asm: cc65 drops a volatile read
 * whose value goes nowhere, so `(void)*(volatile uint8_t *)0xC080` emits
 * nothing. */
#define COGS_LC_ENTER() do { \
        if (cogs_lc_depth++ == 0) { \
            cogs_lc_was_rom = !(*(volatile uint8_t *)0xC012 & 0x80); \
            __asm__ ("bit $C080"); \
        } } while (0)
#define COGS_LC_LEAVE() do { \
        if (--cogs_lc_depth == 0 && cogs_lc_was_rom) \
            __asm__ ("bit $C082"); \
        } while (0)
#else
#define COGS_LC_ENTER() do { } while (0)
#define COGS_LC_LEAVE() do { } while (0)
#endif

#endif /* COGS_CARD_H */
