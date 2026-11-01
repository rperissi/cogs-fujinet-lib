/*
 * appkey_disk.c - FujiNet appkeys as files on the game's ProDOS volume.
 *
 * Copyright (C) 2026 Rob Perissi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. See LICENSE.
 *
 * A FujiNet keeps appkeys on its SD card. The CoGS card has no key store,
 * so each key becomes a small file in the ProDOS prefix (the volume the
 * game booted from): AK<creator4><app2><key2> in hex, file type $F1,
 * one length byte then FN_COGS_APPKEY data bytes (always the full record,
 * so a shorter value never leaves an old tail). On a read-only volume (a
 * catalog stream)
 * the write fails and the caller sees false; on a writable copy the
 * player's name and preferences survive a reboot.
 *
 * Raw MLI, no stdio: the games are within a few hundred bytes of the
 * top of memory. The 1K file buffer OPEN needs is text page 2 ($0800),
 * which nothing in a 40-column hires game touches.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef FN_COGS_APPKEY
#define FN_COGS_APPKEY 64
#endif

uint8_t __fastcall__ fn_mli(uint8_t call, void *params);

#define MLI_CREATE  0xC0
#define MLI_OPEN    0xC8
#define MLI_READ    0xCA
#define MLI_WRITE   0xCB
#define MLI_CLOSE   0xCC

#define IO_BUF      ((void *)0x0800)

/* Language card like the rest of the bus: only fn_mli (mli.s) has to be
 * in main RAM, because ProDOS reads the call bytes after the JSR with its
 * own bank switched in. The MLI restores the bank state on return, so
 * coming back into bank 2 code is fine. Callers hold COGS_LC_ENTER. */
#ifndef FN_COGS_NO_LC
#pragma code-name ("LC")
#endif

static uint8_t path[12];          /* len, "AK" + 8 hex */

static struct {
    uint8_t  count; uint8_t *path; uint8_t access; uint8_t ftype;
    uint16_t aux; uint8_t storage; uint16_t date, time;
} p_create;
static struct { uint8_t count; uint8_t *path; void *buf; uint8_t ref; } p_open;
static struct { uint8_t count, ref; void *buf; uint16_t req, got; } p_rw;
static struct { uint8_t count, ref; } p_close;

static void set_path(const uint8_t *id)
{
    static const char hex[] = "0123456789ABCDEF";
    static uint8_t i, b, *o;
    path[0] = 10;
    path[1] = 'A'; path[2] = 'K';
    o = path + 3;
    /* id = creator lo, hi, app, key, as the bytes arrive */
    for (i = 0; i < 4; i++) {
        b = id[i];
        *o++ = hex[b >> 4];
        *o++ = hex[b & 15];
    }
}

static bool open_key(void)
{
    p_open.count = 3; p_open.path = path; p_open.buf = IO_BUF;
    return fn_mli(MLI_OPEN, &p_open) == 0;
}

static void close_key(void)
{
    p_close.count = 1; p_close.ref = p_open.ref;
    fn_mli(MLI_CLOSE, &p_close);
}

/* rec = len byte + FN_COGS_APPKEY data bytes, in both directions. */
bool fn_appkey_disk_read(const uint8_t *id, uint8_t *rec)
{
    set_path(id);
    if (!open_key())
        return false;
    p_rw.count = 4; p_rw.ref = p_open.ref; p_rw.buf = rec;
    p_rw.req = 1 + FN_COGS_APPKEY; p_rw.got = 0;
    fn_mli(MLI_READ, &p_rw);
    close_key();
    if (p_rw.got < 1 || rec[0] > FN_COGS_APPKEY)
        return false;
    return true;
}

bool fn_appkey_disk_write(const uint8_t *id, const uint8_t *rec)
{
    set_path(id);
    if (!open_key()) {
        p_create.count = 7; p_create.path = path; p_create.access = 0xC3;
        p_create.ftype = 0xF1; p_create.aux = 0; p_create.storage = 1;
        p_create.date = 0; p_create.time = 0;
        if (fn_mli(MLI_CREATE, &p_create) != 0)
            return false;
        if (!open_key())
            return false;
    }
    p_rw.count = 4; p_rw.ref = p_open.ref; p_rw.buf = (void *)rec;
    p_rw.req = 1 + FN_COGS_APPKEY; p_rw.got = 0;
    if (fn_mli(MLI_WRITE, &p_rw) != 0) {
        close_key();
        return false;
    }
    close_key();
    return true;
}
