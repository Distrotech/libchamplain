/*
 * Copyright (C) 2010 Jiri Techet <techet@gmail.com>
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

#define DEBUG_FLAG CHAMPLAIN_DEBUG_CACHE
#include "champlain-debug.h"

#include "champlain-memory-cache.h"

#include <glib.h>
#include <string.h>

G_DEFINE_TYPE (ChamplainMemoryCache, champlain_memory_cache, CHAMPLAIN_TYPE_TILE_CACHE);

#define GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), CHAMPLAIN_TYPE_MEMORY_CACHE, ChamplainMemoryCachePrivate))

enum
{
  PROP_0,
  PROP_SIZE_LIMIT
};

struct _ChamplainMemoryCachePrivate
{
  guint size_limit;
  GQueue *queue;
};

typedef struct
{
  gchar *key;
  gchar *data;
  guint size;
} QueueMember;


static void fill_tile (ChamplainMapSource *map_source,
    ChamplainTile *tile);

static void store_tile (ChamplainTileCache *tile_cache,
    ChamplainTile *tile,
    const gchar *contents,
    gsize size);
static void refresh_tile_time (ChamplainTileCache *tile_cache,
    ChamplainTile *tile);
static void on_tile_filled (ChamplainTileCache *tile_cache,
    ChamplainTile *tile);


static void
champlain_memory_cache_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  ChamplainMemoryCache *memory_cache = CHAMPLAIN_MEMORY_CACHE (object);

  switch (property_id)
    {
    case PROP_SIZE_LIMIT:
      g_value_set_uint (value, champlain_memory_cache_get_size_limit (memory_cache));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
champlain_memory_cache_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  ChamplainMemoryCache *memory_cache = CHAMPLAIN_MEMORY_CACHE (object);

  switch (property_id)
    {
    case PROP_SIZE_LIMIT:
      champlain_memory_cache_set_size_limit (memory_cache, g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
champlain_memory_cache_dispose (GObject *object)
{
  G_OBJECT_CLASS (champlain_memory_cache_parent_class)->dispose (object);
}


static void
champlain_memory_cache_finalize (GObject *object)
{
  ChamplainMemoryCache *memory_cache = CHAMPLAIN_MEMORY_CACHE (object);

  g_queue_free (memory_cache->priv->queue);

  G_OBJECT_CLASS (champlain_memory_cache_parent_class)->finalize (object);
}


static void
champlain_memory_cache_class_init (ChamplainMemoryCacheClass *klass)
{
  ChamplainMapSourceClass *map_source_class = CHAMPLAIN_MAP_SOURCE_CLASS (klass);
  ChamplainTileCacheClass *tile_cache_class = CHAMPLAIN_TILE_CACHE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  g_type_class_add_private (klass, sizeof (ChamplainMemoryCachePrivate));

  object_class->finalize = champlain_memory_cache_finalize;
  object_class->dispose = champlain_memory_cache_dispose;
  object_class->get_property = champlain_memory_cache_get_property;
  object_class->set_property = champlain_memory_cache_set_property;

  /*
   * ChamplainMemoryCache:size-limit:
   *
   * The cache size limit in bytes.
   *
   * Note: this new value will not be applied until you call #champlain_cache_purge
   *
   * Since: 0.4
   */
  pspec = g_param_spec_uint ("size-limit",
      "Size Limit",
      "The cache's size limit (Mb)",
      1,
      G_MAXINT,
      100,
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_SIZE_LIMIT, pspec);

  tile_cache_class->store_tile = store_tile;
  tile_cache_class->refresh_tile_time = refresh_tile_time;
  tile_cache_class->on_tile_filled = on_tile_filled;

  map_source_class->fill_tile = fill_tile;
}


ChamplainMemoryCache *
champlain_memory_cache_new_full (guint size_limit,
    ChamplainRenderer *renderer)
{
  ChamplainMemoryCache *cache;

  cache = g_object_new (CHAMPLAIN_TYPE_MEMORY_CACHE,
      "size-limit", size_limit,
      "renderer", renderer,
      NULL);

  return cache;
}


static void
champlain_memory_cache_init (ChamplainMemoryCache *memory_cache)
{
  ChamplainMemoryCachePrivate *priv = GET_PRIVATE (memory_cache);

  memory_cache->priv = priv;

  priv->queue = g_queue_new ();
}


guint
champlain_memory_cache_get_size_limit (ChamplainMemoryCache *memory_cache)
{
  g_return_val_if_fail (CHAMPLAIN_IS_MEMORY_CACHE (memory_cache), 0);

  return memory_cache->priv->size_limit;
}


void
champlain_memory_cache_set_size_limit (ChamplainMemoryCache *memory_cache,
    guint size_limit)
{
  g_return_if_fail (CHAMPLAIN_IS_MEMORY_CACHE (memory_cache));

  ChamplainMemoryCachePrivate *priv = memory_cache->priv;

  priv->size_limit = size_limit;
  g_object_notify (G_OBJECT (memory_cache), "size-limit");
}


static gchar *
generate_queue_key (ChamplainMemoryCache *memory_cache,
    ChamplainTile *tile)
{
  g_return_val_if_fail (CHAMPLAIN_IS_MEMORY_CACHE (memory_cache), NULL);
  g_return_val_if_fail (CHAMPLAIN_IS_TILE (tile), NULL);

  ChamplainMapSource *map_source = CHAMPLAIN_MAP_SOURCE (memory_cache);

  gchar *filename = g_strdup_printf ("%d/%d/%d/%s",
      champlain_tile_get_zoom_level (tile),
      champlain_tile_get_x (tile),
      champlain_tile_get_y (tile),
      champlain_map_source_get_id (map_source));
  return filename;
}


static gint
compare_queue_members (const QueueMember *a, const QueueMember *b)
{
  return g_strcmp0 (a->key, b->key);
}


static void
move_queue_member_to_head (GQueue *queue, GList *link)
{
  g_queue_unlink (queue, link);
  g_queue_push_head_link (queue, link);
}


static void
delete_queue_member (QueueMember *member, gpointer user_data)
{
  if (member)
    {
      g_free (member->key);
      g_free (member->data);
      g_slice_free (QueueMember, member);
    }
}


static void
tile_rendered_cb (ChamplainTile *tile,
    ChamplainRenderCallbackData *data,
    ChamplainMapSource *map_source)
{
  ChamplainMapSource *next_source = champlain_map_source_get_next_source (map_source);

  if (!data->error)
    {
      if (CHAMPLAIN_IS_TILE_CACHE (next_source))
        on_tile_filled (CHAMPLAIN_TILE_CACHE (next_source), tile);

      champlain_tile_set_fade_in (tile, TRUE);
      champlain_tile_set_state (tile, CHAMPLAIN_STATE_DONE);
      champlain_tile_display_content (tile);
    }
  else if (CHAMPLAIN_IS_MAP_SOURCE (next_source))
    champlain_map_source_fill_tile (next_source, tile);

  g_object_unref (map_source);
  g_signal_handlers_disconnect_by_func (tile, tile_rendered_cb, map_source);
}


static void
fill_tile (ChamplainMapSource *map_source,
    ChamplainTile *tile)
{
  g_return_if_fail (CHAMPLAIN_IS_MEMORY_CACHE (map_source));
  g_return_if_fail (CHAMPLAIN_IS_TILE (tile));

  ChamplainMapSource *next_source = champlain_map_source_get_next_source (map_source);

  if (champlain_tile_get_state (tile) != CHAMPLAIN_STATE_LOADED)
    {
      ChamplainMemoryCache *memory_cache = CHAMPLAIN_MEMORY_CACHE (map_source);
      ChamplainMemoryCachePrivate *priv = memory_cache->priv;
      ChamplainRenderer *renderer;
      GList *link;
      QueueMember key;

      key.key = generate_queue_key (memory_cache, tile);
      link = g_queue_find_custom (priv->queue, &key, (GCompareFunc) compare_queue_members);
      if (link)
        {
          QueueMember *member = link->data;

          move_queue_member_to_head (priv->queue, link);

          renderer = champlain_map_source_get_renderer (map_source);

          g_return_if_fail (CHAMPLAIN_IS_RENDERER (renderer));

          g_object_ref (map_source);
          g_signal_connect (tile, "render-complete", G_CALLBACK (tile_rendered_cb), map_source);
          champlain_renderer_set_data (renderer, member->data, member->size);
          champlain_renderer_render (renderer, tile);

          return;
        }
    }

  if (CHAMPLAIN_IS_MAP_SOURCE (next_source))
    champlain_map_source_fill_tile (next_source, tile);
  else if (champlain_tile_get_state (tile) == CHAMPLAIN_STATE_LOADED)
    {
      /* if we have some content, use the tile even if it wasn't validated */
      champlain_tile_set_state (tile, CHAMPLAIN_STATE_DONE);
      champlain_tile_display_content (tile);
    }
}


static void
store_tile (ChamplainTileCache *tile_cache,
    ChamplainTile *tile,
    const gchar *contents,
    gsize size)
{
  g_return_if_fail (CHAMPLAIN_IS_MEMORY_CACHE (tile_cache));

  ChamplainMemoryCache *memory_cache = CHAMPLAIN_MEMORY_CACHE (tile_cache);
  ChamplainMemoryCachePrivate *priv = memory_cache->priv;
  GList *link;
  QueueMember key;

  key.key = generate_queue_key (memory_cache, tile);
  link = g_queue_find_custom (priv->queue, &key, (GCompareFunc) compare_queue_members);
  if (link)
    move_queue_member_to_head (priv->queue, link);
  else
    {
      QueueMember *member;

      if (priv->queue->length >= priv->size_limit)
        {
          member = g_queue_pop_tail (priv->queue);
          delete_queue_member (member, NULL);
        }

      member = g_slice_new (QueueMember);
      member->key = key.key;
      member->data = g_memdup (contents, size);
      member->size = size;

      g_queue_push_head (priv->queue, member);
    }
}


static void
refresh_tile_time (ChamplainTileCache *tile_cache,
    ChamplainTile *tile)
{
}


void
champlain_memory_cache_clean (ChamplainMemoryCache *memory_cache)
{
  ChamplainMemoryCachePrivate *priv = memory_cache->priv;

  g_queue_foreach (priv->queue, (GFunc) delete_queue_member, NULL);
  g_queue_clear (priv->queue);
}


static void
on_tile_filled (ChamplainTileCache *tile_cache,
    ChamplainTile *tile)
{
  g_return_if_fail (CHAMPLAIN_IS_MEMORY_CACHE (tile_cache));
  g_return_if_fail (CHAMPLAIN_IS_TILE (tile));

  ChamplainMapSource *map_source = CHAMPLAIN_MAP_SOURCE (tile_cache);
  ChamplainMapSource *next_source = champlain_map_source_get_next_source (map_source);
  ChamplainMemoryCache *memory_cache = CHAMPLAIN_MEMORY_CACHE (tile_cache);
  ChamplainMemoryCachePrivate *priv = memory_cache->priv;
  GList *link;
  QueueMember key;

  key.key = generate_queue_key (memory_cache, tile);
  link = g_queue_find_custom (priv->queue, &key, (GCompareFunc) compare_queue_members);
  if (link)
    move_queue_member_to_head (priv->queue, link);

  if (CHAMPLAIN_IS_TILE_CACHE (next_source))
    champlain_tile_cache_on_tile_filled (CHAMPLAIN_TILE_CACHE (next_source), tile);
}