/*
 * fujinet-bus-cogs.c - fujinet-lib bus backend for the CoGS card.
 *
 * Copyright (C) 2026 Rob Perissi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. See LICENSE.
 *
 * fujinet-lib's common/ code talks to the FujiNet through four hooks:
 *
 *   fuji_bus_call(device, cmd, fields, aux..., [data,len], [reply,len])
 *   network_bus_read(device, buf, len) / network_bus_write(...)
 *   fuji_bus_appkey_read(buf, &len) / fuji_bus_appkey_write(buf, len)
 *
 * On a real FujiNet those are SmartPort CONTROL/STATUS/READ/WRITE calls
 * to a device that owns a TCP stack and a JSON parser. This file answers
 * the same calls with a CoGS card in a slot instead: an HTTP(S) open
 * becomes one HTTP frame to the card, a JSON query becomes JSON_GET on
 * the body the card retained, reads come out of a small per-unit buffer.
 * Nothing in fujinet-lib/common changes, so applications written for the
 * FujiNet network device rebuild against this library unmodified.
 *
 * Scope of this first cut (an Apple II has 48K to share with the game,
 * so everything here is sized for the lobby games):
 *   NETWORK  OPEN (http/https; GET, POST, PUT, DELETE), CLOSE, STATUS,
 *            SET_PARSER + PARSE + QUERY (JSON), SET_HTTP_MODE (headers,
 *            POST body), read, write, GET_ERROR; SET_EOL, TRANSLATION,
 *            SET_CHANNEL, SET_PARAMETER, USERNAME, PASSWORD are accepted
 *            and ignored. tcp:// udp:// tnfs:// are not here yet.
 *   FUJINET  GET_WIFISTATUS, RANDOM_NUMBER, OPEN/READ/WRITE_APPKEY
 *            (kept in RAM for the session; the card has no key store and
 *            a catalog-streamed boot disk is read-only), RESET no-op.
 */
#include <string.h>
#include <stdarg.h>
#include <fujinet-network.h>
#include <fujinet-fuji.h>
#include <fujinet-appkey.h>
#include <fujinet-err.h>
#include "cogs_card.h"

#ifndef FN_COGS_UNITS
#define FN_COGS_UNITS 1          /* N: (unit 1) only, by default */
#endif
#ifndef FN_COGS_BUF
#define FN_COGS_BUF 768          /* per-unit body / query / POST buffer */
#endif
#ifndef FN_COGS_URL
#define FN_COGS_URL 128
#endif
#ifndef FN_COGS_HDRS
#define FN_COGS_HDRS 0           /* request header collection: bytes of
                                    buffer, 0 = headers accepted and
                                    dropped (saves ~400 bytes) */
#endif
#ifndef FN_COGS_APPKEY
#define FN_COGS_APPKEY 64
#endif

enum { ST_CLOSED = 0, ST_OPEN, ST_FETCHED, ST_RETAINED };

typedef struct {
    uint8_t  state;
    uint8_t  mode;        /* OPEN_MODE_HTTP_* */
    uint8_t  trans;       /* OPEN_TRANS_* */
    uint8_t  json;        /* SET_PARSER(PARSER_JSON) seen */
    uint8_t  chan;        /* HTTP_CHAN_MODE_* */
    uint8_t  err;         /* NETWORK_* errcode reported by STATUS */
    uint16_t len;         /* bytes in buf */
    uint16_t pos;         /* read cursor */
    uint8_t  hdrs_len;
    char     url[FN_COGS_URL];
#if FN_COGS_HDRS
    char     hdrs[FN_COGS_HDRS];
#endif
    uint8_t  buf[FN_COGS_BUF];
} nunit_t;

static nunit_t units[FN_COGS_UNITS];
static uint8_t online;
uint8_t fn_device_error;

/* scratch shared by the frame readers (cc65 locals are expensive) */
static uint8_t  f_op, f_sock, f_code;
static uint16_t f_len, f_n, f_room;

uint8_t fn_error(uint8_t code)
{
    fn_device_error = code;
    return code ? FN_ERR_IO_ERROR : FN_ERR_OK;
}

static nunit_t *unit_for(uint8_t device)
{
    device -= FUJI_DEVICEID_NETWORK;
    return device < FN_COGS_UNITS ? &units[device] : 0;
}

#define SOCK(u) ((uint8_t)((u) - units))

static uint8_t map_card_err(uint8_t code)
{
    switch (code) {
    case COGS_E_OK:            return NETWORK_SUCCESS;
    case COGS_E_NOT_JOINED:    return NETWORK_ERROR_NETWORK_DOWN;
    case COGS_E_DNS_FAIL:      return NETWORK_ERROR_NETWORK_UNREACHABLE;
    case COGS_E_CONN_TIMEOUT:  return NETWORK_ERROR_SOCKET_TIMEOUT;
    case COGS_E_TLS_HANDSHAKE:
    case COGS_E_CERT_VERIFY:   return NETWORK_ERROR_CONNECTION_REFUSED;
    case COGS_E_NO_RETAINED:
    case COGS_E_JSON_NOPATH:   return NETWORK_ERROR_FILE_NOT_FOUND;
    }
    return NETWORK_ERROR_GENERAL;
}

/* Pull f_len payload bytes of a DATA / JSON chunk into buf, truncating
 * at the buffer. Returns nonzero when something was dropped. */
static uint8_t take_into_buf(nunit_t *u)
{
    f_room = FN_COGS_BUF - u->len;
    f_n = f_len < f_room ? f_len : f_room;
    if (f_n)
        cogs_rx_bytes(u->buf + u->len, f_n);
    u->len += f_n;
    cogs_rx_skip(f_len - f_n);
    return f_n < f_len;
}

/* Run the unit's HTTP transaction. body/body_len for POST/PUT. With
 * retain the card keeps the body for JSON_GET; otherwise $91 DATA events
 * fill buf. */
static bool do_http(nunit_t *u, const uint8_t *body, uint16_t body_len, uint8_t retain)
{
    static const char *method;
    static uint8_t mlen, ulen, truncated;

    if (!online) {
        if (cogs_card_online() != COGS_OK) {
            u->err = NETWORK_ERROR_NETWORK_DOWN;
            u->state = ST_FETCHED;
            return false;
        }
        online = 1;
    }

    switch (u->mode) {
    case OPEN_MODE_HTTP_POST:     method = "POST"; break;
    case OPEN_MODE_HTTP_PUT:
    case OPEN_MODE_HTTP_PUT_H:    method = "PUT"; break;
    case OPEN_MODE_HTTP_DELETE:
    case OPEN_MODE_HTTP_DELETE_H: method = "DELETE"; break;
    default:                      method = "GET";
    }
    mlen = (uint8_t)strlen(method);
    ulen = (uint8_t)strlen(u->url);

    /* sock flags mlen method ulen url hdrs_len(2) hdrs body_len(2) body */
    cogs_tx_begin(COGS_CMD_HTTP, 8 + mlen + ulen + u->hdrs_len + body_len);
    cogs_tx_byte(SOCK(u));
    cogs_tx_byte(COGS_HTTP_KEEPALIVE | (retain ? COGS_HTTP_RETAIN : 0));
    cogs_tx_byte(mlen);
    cogs_tx_bytes(method, mlen);
    cogs_tx_byte(ulen);
    cogs_tx_bytes(u->url, ulen);
    cogs_tx_u16(u->hdrs_len);
#if FN_COGS_HDRS
    cogs_tx_bytes(u->hdrs, u->hdrs_len);
#endif
    cogs_tx_u16(body_len);
    cogs_tx_bytes(body, body_len);

    u->len = 0;
    u->pos = 0;
    truncated = 0;

    for (;;) {
        if (cogs_rx_header(&f_op, &f_len) != COGS_OK) {
            u->err = NETWORK_ERROR_GENERAL_TIMEOUT;
            u->state = ST_FETCHED;
            return false;
        }
        if (f_op == COGS_EVT_DATA && f_len) {
            f_sock = cogs_rx_byte();
            f_len--;
            if (f_sock == SOCK(u) && !retain)
                truncated |= take_into_buf(u);
            else
                cogs_rx_skip(f_len);
        } else if (f_op == COGS_EVT_HTTP_DONE && f_len >= 7) {
            f_sock = cogs_rx_byte();
            f_n = cogs_rx_byte();
            f_n |= (uint16_t)cogs_rx_byte() << 8;      /* HTTP status */
            cogs_rx_skip(f_len - 3);
            if (f_sock != SOCK(u))
                continue;
            if (f_n == 404)      u->err = NETWORK_ERROR_FILE_NOT_FOUND;
            else if (f_n >= 400) u->err = NETWORK_ERROR_GENERAL;
            else if (truncated)  u->err = NETWORK_ERROR_NO_SPACE_ON_DEVICE;
            else                 u->err = NETWORK_SUCCESS;
            u->state = retain ? ST_RETAINED : ST_FETCHED;
            return true;
        } else if (f_op == COGS_EVT_ERROR && f_len >= 2) {
            f_sock = cogs_rx_byte();
            f_code = cogs_rx_byte();
            cogs_rx_skip(f_len - 2);
            if (f_sock != SOCK(u) && f_sock != 0xFF)
                continue;
            u->err = map_card_err(f_code);
            u->state = ST_FETCHED;
            return false;
        } else {
            cogs_rx_skip(f_len);
        }
    }
}

/* FujiNet does the GET at open; we wait for the first read or status so
 * a following SET_PARSER(JSON) can send the body to the card's retain
 * buffer instead of ours. */
static void ensure_fetched(nunit_t *u)
{
    if (u->state == ST_OPEN)
        do_http(u, 0, 0, u->json);
}

/* The JSON and fuji-device paths run from the language card (bank 2,
 * $D400-$DFFF, the cc65 "LC" segment the Apple II configs already carry)
 * so they cost the game no main RAM. cc65's crt0 copies the segment up
 * at start and keeps the bank enabled; ProDOS restores it around MLI
 * calls. Opt out with -DFN_COGS_NO_LC if an app's linker config lacks LC. */
#ifndef FN_COGS_NO_LC
#pragma code-name (push, "LC")
#endif

/* FujiNet queries are JSON pointers, "/players/0/name"; the card walks
 * "players[0].name". Anything not starting with '/' passes through. */
static uint8_t translate_path(const char *in, char *out)
{
    static uint8_t o, seglen, numeric, first;
    static const char *p, *seg;

    o = 0;
    if (*in != '/') {
        while (*in && o < 95)
            out[o++] = *in++;
        out[o] = 0;
        return o;
    }
    first = 1;
    p = in + 1;
    while (*p && o < 90) {
        seg = p;
        numeric = 1;
        seglen = 0;
        while (*p && *p != '/') {
            if (*p < '0' || *p > '9')
                numeric = 0;
            p++;
            seglen++;
        }
        if (seglen && numeric) {
            out[o++] = '[';
            memcpy(out + o, seg, seglen); o += seglen;
            out[o++] = ']';
        } else {
            if (!first)
                out[o++] = '.';
            memcpy(out + o, seg, seglen); o += seglen;
        }
        first = 0;
        if (*p == '/')
            p++;
    }
    out[o] = 0;
    return o;
}

static bool do_json_query(nunit_t *u, const char *query)
{
    static uint8_t plen, result;
    static uint16_t value_len;

    /* The query string is NUL terminated (fujinet-lib passes the caller's
     * buffer with a fixed length). Build the card path in buf, which is
     * free until the reply arrives. */
    plen = translate_path(query, (char *)u->buf);

    cogs_tx_begin(COGS_CMD_JSON_GET, 2 + plen);
    cogs_tx_byte(SOCK(u));
    cogs_tx_byte(plen);
    cogs_tx_bytes(u->buf, plen);

    u->len = 0;
    u->pos = 0;
    for (;;) {
        if (cogs_rx_header(&f_op, &f_len) != COGS_OK) {
            u->err = NETWORK_ERROR_GENERAL_TIMEOUT;
            return false;
        }
        if (f_op == COGS_RSP(COGS_CMD_JSON_GET) && f_len >= 3) {
            result = cogs_rx_byte();
            value_len = cogs_rx_byte();
            value_len |= (uint16_t)cogs_rx_byte() << 8;
            f_len -= 3;
            if (take_into_buf(u) || result != COGS_E_OK)
                break;
            if (u->len >= value_len)
                break;
        } else if (f_op == COGS_EVT_ERROR && f_len >= 2) {
            f_sock = cogs_rx_byte();
            f_code = cogs_rx_byte();
            cogs_rx_skip(f_len - 2);
            if (f_sock == SOCK(u) || f_sock == 0xFF) {
                u->err = map_card_err(f_code);
                return false;
            }
        } else {
            cogs_rx_skip(f_len);
        }
    }
    if (result != COGS_E_OK) {
        u->err = map_card_err(result);
        u->len = 0;
        return false;
    }
    /* FujiNet renders JSON literals in upper case */
    if (u->len == 4 && !memcmp(u->buf, "true", 4))       memcpy(u->buf, "TRUE", 4);
    else if (u->len == 5 && !memcmp(u->buf, "false", 5)) memcpy(u->buf, "FALSE", 5);
    else if (u->len == 4 && !memcmp(u->buf, "null", 4))  memcpy(u->buf, "NULL", 4);
    u->err = NETWORK_SUCCESS;
    return true;
}

#ifndef FN_COGS_NO_LC
#pragma code-name (pop)
#endif

static bool net_call(nunit_t *u, uint8_t cmd, uint8_t a1, uint8_t a2,
                     const uint8_t *data, uint16_t data_len,
                     uint8_t *reply, uint16_t reply_len)
{
    switch (cmd) {
    case NETCMD_OPEN:
        /* a1 = mode, a2 = trans, data = devicespec "N:https://..." */
        if (!data_len)
            return false;
        if (data[1] == ':')      { data += 2; data_len -= 2; }
        else if (data[2] == ':') { data += 3; data_len -= 3; }
        while (data_len && data[data_len - 1] == 0) data_len--;
        if (data_len >= FN_COGS_URL) data_len = FN_COGS_URL - 1;
        memcpy(u->url, data, data_len);
        u->url[data_len] = 0;
        u->state = ST_OPEN;
        u->mode  = a1;
        u->trans = a2;
        u->json  = 0;
        u->chan  = HTTP_CHAN_MODE_BODY;
        u->err   = NETWORK_SUCCESS;
        u->len = u->pos = 0;
        u->hdrs_len = 0;
        return true;

    case NETCMD_CLOSE:
        u->state = ST_CLOSED;
        return true;

    case NETCMD_STATUS: {
        NetworkStatus *s = (NetworkStatus *)reply;
        if (!reply || reply_len < sizeof(NetworkStatus))
            return false;
        if (u->state == ST_CLOSED) {
            s->avail = 0;
            s->status = 0;
            s->errcode = NETWORK_ERROR_GENERAL;
            return true;
        }
        if (!u->json)
            ensure_fetched(u);
        s->avail = u->len - u->pos;
        s->status = 1;
        if (s->avail)                            s->errcode = NETWORK_SUCCESS;
        else if (u->err != NETWORK_SUCCESS)      s->errcode = u->err;
        else                                     s->errcode = NETWORK_ERROR_END_OF_FILE;
        return true;
    }
    case NETCMD_GET_ERROR:
        if (reply && reply_len)
            reply[0] = u->err;
        return true;

    case NETCMD_SET_PARSER:
        u->json = (a1 == PARSER_JSON);
        return true;

    case NETCMD_PARSE:
    case NETCMD_PARSE_ALT:
        if (u->state == ST_OPEN && u->json)
            return do_http(u, 0, 0, 1);
        return u->state == ST_RETAINED;

    case NETCMD_QUERY:
    case NETCMD_QUERY_ALT:
        if (u->state != ST_RETAINED)
            return false;
        return do_json_query(u, (const char *)data);

    case NETCMD_SET_HTTP_MODE:
        /* a2 = HTTP_CHAN_MODE_* */
        if (u->state == ST_CLOSED)
            return false;
        if (a2 == HTTP_CHAN_MODE_POST_SET_DATA)
            u->len = u->pos = 0;                 /* buf collects the body */
        if (a2 == HTTP_CHAN_MODE_BODY && u->chan == HTTP_CHAN_MODE_POST_SET_DATA
            && u->state == ST_OPEN) {
            u->chan = HTTP_CHAN_MODE_BODY;
            /* do_http sends buf as the body before it reuses buf */
            return do_http(u, u->buf, u->len, u->json);
        }
        u->chan = (a2 == HTTP_CHAN_MODE_SET_HEADERS || a2 == HTTP_CHAN_MODE_POST_SET_DATA)
                  ? a2 : HTTP_CHAN_MODE_BODY;
        return true;

    case NETCMD_SET_EOL:
    case NETCMD_TRANSLATION:
    case NETCMD_SET_CHANNEL:
    case NETCMD_SET_PARAMETER:
    case NETCMD_USERNAME:
    case NETCMD_PASSWORD:
        return true;
    }
    return false;
}

/* ---- fuji device --------------------------------------------------------- */

#ifndef FN_COGS_NO_LC
#pragma code-name (push, "LC")
#endif

static uint8_t ak_id[4];          /* creator lo, hi, app, key of the open key */
static uint8_t ak_ram_id[4];
static uint8_t ak_ram[FN_COGS_APPKEY];
static uint8_t ak_ram_len, ak_ram_valid;

bool fuji_bus_appkey_read(void *string, uint16_t *length)
{
    if (ak_ram_valid && !memcmp(ak_id, ak_ram_id, 4)) {
        memcpy(string, ak_ram, ak_ram_len);
        *length = ak_ram_len;
        return true;
    }
    *length = 0;
    return false;
}

bool fuji_bus_appkey_write(const void *string, uint16_t length)
{
    if (length > FN_COGS_APPKEY) length = FN_COGS_APPKEY;
    memcpy(ak_ram_id, ak_id, 4);
    memcpy(ak_ram, string, length);
    ak_ram_len = (uint8_t)length;
    ak_ram_valid = 1;
    return true;
}

static bool fuji_call(uint8_t cmd, const uint8_t *data, uint16_t data_len,
                      uint8_t *reply, uint16_t reply_len)
{
    switch (cmd) {
    case FUJICMD_GET_WIFISTATUS:
        if (!cogs_card_find())
            return false;
        if (reply && reply_len)
            reply[0] = (*(volatile uint8_t *)(cogs_base + 1) & COGS_ST_LINK_UP) ? 3 : 6;
        return true;

    case FUJICMD_RANDOM_NUMBER:
        if (!reply || !reply_len || reply_len > 255 || !cogs_card_find())
            return false;
        cogs_tx_begin(COGS_CMD_RANDOM, 1);
        cogs_tx_byte((uint8_t)reply_len);
        return cogs_wait_reply(COGS_RSP(COGS_CMD_RANDOM), reply, reply_len, &f_n, 0) == COGS_OK;

    case FUJICMD_OPEN_APPKEY:
        /* FNAppKeyID { creator u16, app_id, key_id, mode, reserved } */
        if (!data || data_len < 4)
            return false;
        memcpy(ak_id, data, 4);
        return true;

    case FUJICMD_READ_APPKEY:
        return reply && fuji_bus_appkey_read(reply, &f_n);

    case FUJICMD_WRITE_APPKEY:
        return fuji_bus_appkey_write(data, data_len);

    case FUJICMD_RESET:
        return true;
    }
    return false;
}

#ifndef FN_COGS_NO_LC
#pragma code-name (pop)
#endif

/* ---- the bus entry points ------------------------------------------------ */

bool fuji_bus_call(uint8_t device, uint8_t fuji_cmd, uint8_t fields, ...)
{
    static va_list  ap;
    static uint8_t  numbytes, i, aux[4];
    static const uint8_t *data;
    static uint8_t *reply;
    static uint16_t data_len, reply_len;
    static bool ok;
    nunit_t *u;

    numbytes = fuji_field_numbytes(fields);
    aux[0] = aux[1] = 0;
    data = 0; data_len = 0;
    reply = 0; reply_len = 0;

    va_start(ap, fields);
    for (i = 0; i < numbytes; i++)
        aux[i] = (uint8_t)va_arg(ap, int);
    if (fields & FUJI_FIELD_DATA) {
        data = va_arg(ap, const uint8_t *);
        data_len = va_arg(ap, unsigned int);
    }
    if (fields & FUJI_FIELD_REPLY) {
        reply = va_arg(ap, uint8_t *);
        reply_len = va_arg(ap, unsigned int);
    }
    va_end(ap);

    if (device == FUJI_DEVICEID_FUJINET) {
        ok = fuji_call(fuji_cmd, data, data_len, reply, reply_len);
    } else {
        u = unit_for(device);
        ok = u ? net_call(u, fuji_cmd, aux[0], aux[1], data, data_len, reply, reply_len) : false;
    }
    fn_device_error = !ok;
    return ok;
}

size_t network_bus_read(uint8_t device, void *buffer, size_t length)
{
    nunit_t *u = unit_for(device);
    uint8_t *d = (uint8_t *)buffer;

    if (!u || u->state == ST_CLOSED)
        return 0;
    if (!u->json)
        ensure_fetched(u);
    f_room = u->len - u->pos;
    f_n = length < f_room ? (uint16_t)length : f_room;
    memcpy(d, u->buf + u->pos, f_n);
    u->pos += f_n;
    return f_n;
}

size_t network_bus_write(uint8_t device, const void *buffer, size_t length)
{
    nunit_t *u = unit_for(device);
    const uint8_t *s = (const uint8_t *)buffer;

    if (!u || u->state == ST_CLOSED)
        return 0;
    if (u->chan == HTTP_CHAN_MODE_SET_HEADERS) {
#if FN_COGS_HDRS
        /* One header per write; join with CRLF for the card. */
        if (length + 2 > (uint16_t)(FN_COGS_HDRS - u->hdrs_len))
            return 0;
        memcpy(u->hdrs + u->hdrs_len, s, length);
        u->hdrs_len += (uint8_t)length;
        if (length < 2 || s[length - 2] != '\r') {
            u->hdrs[u->hdrs_len++] = '\r';
            u->hdrs[u->hdrs_len++] = '\n';
        }
#endif
        return length;
    }
    if (u->chan == HTTP_CHAN_MODE_POST_SET_DATA) {
        f_room = FN_COGS_BUF - u->len;
        f_n = length < f_room ? (uint16_t)length : f_room;
        memcpy(u->buf + u->len, s, f_n);
        u->len += f_n;
        return f_n;
    }
    return 0;
}
