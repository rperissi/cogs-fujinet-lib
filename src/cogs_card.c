/*
 * cogs_card.c - minimal CoGS card driver for the fujinet-lib CoGS bus.
 *
 * Copyright (C) 2026 Rob Perissi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. See LICENSE.
 *
 * cc65, Apple II (-t apple2 / apple2enh). Plain 8-bit absolute access to
 * the slot registers; nothing here is 65816 specific, so the same object
 * runs on a IIe, IIc-with-slot carriers and a IIgs in emulation mode.
 */
#include <string.h>
#include "cogs_card.h"

/* Runs from the language card; see the note in fujinet-bus-cogs.c. */
#ifndef FN_COGS_NO_LC
#pragma code-name ("LC")
#endif

uint16_t cogs_base = 0;
uint8_t  cogs_slot = 0;

#define REG_DATA    (*(volatile uint8_t *)(cogs_base + 0))
#define REG_STATUS  (*(volatile uint8_t *)(cogs_base + 1))
#define REG_ID      (*(volatile uint8_t *)(cogs_base + 5))
#define REG_VERSION (*(volatile uint8_t *)(cogs_base + 6))

#define COGS_ID_BYTE 0xA2

/* Backstop for a reply that never comes. The inner spin is ~65k register
 * polls (about two seconds at 1 MHz); the outer count sets the total.
 * The card times out its own DNS/TCP/TLS work and reports $93, so this
 * only has to catch a card that has gone silent. Keep it long: a tight
 * value misfires under an emulator running faster than real time. */
#ifndef COGS_RX_TIMEOUT_OUTER
#define COGS_RX_TIMEOUT_OUTER 250
#endif

uint8_t cogs_card_find(void)
{
    uint8_t s;
#ifdef COGS_SLOT
    s = COGS_SLOT;
    cogs_base = 0xC080 + (uint16_t)s * 16;
    if (REG_ID == COGS_ID_BYTE && REG_VERSION != 0 && REG_STATUS != 0xFF) {
        cogs_slot = s;
        return s;
    }
#else
    if (cogs_slot) {
        /* Re-check the remembered slot; a reset does not move the card. */
        if (REG_ID == COGS_ID_BYTE && REG_VERSION != 0)
            return cogs_slot;
    }
    for (s = 7; s >= 1; s--) {
        cogs_base = 0xC080 + (uint16_t)s * 16;
        if (REG_ID == COGS_ID_BYTE && REG_VERSION != 0 && REG_STATUS != 0xFF) {
            cogs_slot = s;
            return s;
        }
    }
#endif
    cogs_base = 0;
    cogs_slot = 0;
    return 0;
}

/* ---- TX ---------------------------------------------------------------- */

static void tx_raw(uint8_t b)
{
    while (!(REG_STATUS & COGS_ST_TX_READY))
        ;
    REG_DATA = b;
}

void cogs_tx_byte(uint8_t b)
{
    tx_raw(b);
}

void cogs_tx_u16(uint16_t v)
{
    tx_raw((uint8_t)(v & 0xFF));
    tx_raw((uint8_t)(v >> 8));
}

void cogs_tx_begin(uint8_t op, uint16_t len)
{
    tx_raw(op);
    cogs_tx_u16(len);
}

void cogs_tx_bytes(const void *p, uint16_t len)
{
    const uint8_t *s = (const uint8_t *)p;
    while (len--)
        tx_raw(*s++);
}

/* ---- RX ---------------------------------------------------------------- */

/* Wait for a byte with the backstop timeout. Returns -1 on timeout. */
static int16_t rx_wait(void)
{
    uint8_t  outer = COGS_RX_TIMEOUT_OUTER;
    uint16_t inner;
    do {
        inner = 0;
        do {
            if (REG_STATUS & COGS_ST_RX_AVAIL)
                return REG_DATA;
        } while (++inner);
    } while (--outer);
    return -1;
}

uint8_t cogs_rx_byte(void)
{
    /* Inside a frame the card has already queued the whole payload, so a
     * short wait is enough; use the full backstop anyway, it costs nothing
     * on the happy path. */
    int16_t b = rx_wait();
    return b < 0 ? 0 : (uint8_t)b;
}

void cogs_rx_bytes(void *p, uint16_t len)
{
    uint8_t *d = (uint8_t *)p;
    while (len--)
        *d++ = cogs_rx_byte();
}

void cogs_rx_skip(uint16_t len)
{
    while (len--)
        cogs_rx_byte();
}

int8_t cogs_rx_header(uint8_t *op, uint16_t *len)
{
    int16_t b = rx_wait();
    if (b < 0)
        return COGS_ERR_TIMEOUT;
    *op  = (uint8_t)b;
    *len = cogs_rx_byte();
    *len |= (uint16_t)cogs_rx_byte() << 8;
    return COGS_OK;
}

int8_t cogs_wait_reply(uint8_t want, void *out, uint16_t cap,
                       uint16_t *got, uint8_t *errcode)
{
    uint8_t  op;
    uint16_t len;
    uint8_t *d = (uint8_t *)out;

    if (errcode)
        *errcode = COGS_E_OK;
    for (;;) {
        if (cogs_rx_header(&op, &len) != COGS_OK)
            return COGS_ERR_TIMEOUT;
        if (op == want) {
            uint16_t n = len < cap ? len : cap;
            if (got)
                *got = len;
            if (d && n)
                cogs_rx_bytes(d, n);
            cogs_rx_skip(len - n);
            return COGS_OK;
        }
        if (op == COGS_EVT_ERROR && len >= 2) {
            uint8_t sock = cogs_rx_byte();
            uint8_t code = cogs_rx_byte();
            cogs_rx_skip(len - 2);
            if (sock == 0xFF) {
                if (errcode)
                    *errcode = code;
                return COGS_ERR_PROTO;
            }
            continue;           /* some socket's error, not ours */
        }
        cogs_rx_skip(len);      /* event we do not care about */
    }
}

/* ---- link bring-up ----------------------------------------------------- */

int8_t cogs_card_online(void)
{
    uint8_t  res;
    uint16_t got;
    uint8_t  t[4];
    uint8_t  tries;

    if (!cogs_card_find())
        return COGS_ERR_NOCARD;

    if (!(REG_STATUS & COGS_ST_LINK_UP)) {
        /* WIFI_JOIN with two zero lengths = the card's stored credentials */
        cogs_tx_begin(COGS_CMD_WIFI_JOIN, 2);
        cogs_tx_byte(0);
        cogs_tx_byte(0);
        if (cogs_wait_reply(COGS_RSP(COGS_CMD_WIFI_JOIN), &res, 1, &got, 0) != COGS_OK)
            return COGS_ERR_TIMEOUT;
        if (res != 0)
            return COGS_ERR_PROTO;
    }

    /* Verified TLS needs the card clock. TIME answers 0 until NTP ran. */
    for (tries = 0; tries < 40; tries++) {
        cogs_tx_begin(COGS_CMD_TIME, 0);
        if (cogs_wait_reply(COGS_RSP(COGS_CMD_TIME), t, 4, &got, 0) != COGS_OK)
            return COGS_ERR_TIMEOUT;
        if (t[0] | t[1] | t[2] | t[3])
            return COGS_OK;
        {   /* ~0.4 s pause between polls */
            uint16_t spin = 0;
            do { } while (++spin);
            spin = 0;
            do { } while (++spin);
        }
    }
    return COGS_ERR_TIMEOUT;
}
