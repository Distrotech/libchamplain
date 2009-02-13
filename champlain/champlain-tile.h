/*
 * Copyright (C) 2008 Pierre-Luc Beaudoin <pierre-luc@pierlux.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef CHAMPLAIN_MAP_TILE_H
#define CHAMPLAIN_MAP_TILE_H

#include <champlain/champlain-private.h>

#include <glib.h>
#include <clutter/clutter.h>

struct _Tile
{
  ClutterActor* actor;
  int x;
  int y;
  int size;
  int level;

  gboolean loading; // TRUE when a callback exist to load the tile, FALSE otherwise
  gboolean to_destroy; // TRUE when a tile struct should be deleted when loading is done, FALSE otherwise
};

void tile_free(Tile* tile);

void tile_set_position(Map* map, Tile* tile);

Tile* tile_load (Map* map, guint zoom_level, guint x, guint y, gboolean offline);

#endif