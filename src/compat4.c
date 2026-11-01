/*
 * compat4.c - fujinet-lib 4.x function-style entry points.
 *
 * Copyright (C) 2026 Rob Perissi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. See LICENSE.
 *
 * Some FujiNet apps (Battleship, for one) ship a 4.x fujinet-fuji.h in
 * their own source tree, where these were real functions. fujinet-lib 5
 * turned them into macros, so a 4.x-header app linked against a 5.x
 * library misses the symbols. These wrappers resolve them to the 5.x
 * common code.
 */
#include <fujinet-fuji.h>

#undef fuji_set_appkey_details
#undef fuji_read_appkey

/* Main RAM on purpose: apps call these directly, and an app may have
 * switched the language card out (see COGS_LC_ENTER in cogs_card.h). The
 * common code they reach goes through the guarded fuji_bus_call. */

void fuji_set_appkey_details(uint16_t creator_id, uint8_t app_id, enum AppKeySize keysize)
{
    ak_creator_id = creator_id;
    ak_app_id = app_id;
    ak_appkey_size = keysize;
}

bool fuji_read_appkey(uint8_t key_id, uint16_t *count, uint8_t *data)
{
    return fuji_read_appkey_common(key_id, count, data);
}
