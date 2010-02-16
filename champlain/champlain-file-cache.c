/*
 * Copyright (C) 2009 Pierre-Luc Beaudoin <pierre-luc@pierlux.com>
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

/**
 * SECTION:champlain-file-cache
 * @short_description: Stores and loads cached tiles from the file system
 *
 * #ChamplainFileCache is a map source that stores and retrieves tiles from the
 * file system. It can be temporary (deleted when the object is destroyed) or
 * permanent. Tiles most frequently loaded gain in "popularity". This popularity
 * is taken into account when purging the cache.
 */

#define DEBUG_FLAG CHAMPLAIN_DEBUG_CACHE
#include "champlain-debug.h"

#include "champlain-file-cache.h"

#include <sqlite3.h>
#include <errno.h>
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>

G_DEFINE_TYPE (ChamplainFileCache, champlain_file_cache, CHAMPLAIN_TYPE_TILE_CACHE);

#define GET_PRIVATE(obj)    (G_TYPE_INSTANCE_GET_PRIVATE((obj), CHAMPLAIN_TYPE_FILE_CACHE, ChamplainFileCachePrivate))

enum
{
  PROP_0,
  PROP_SIZE_LIMIT,
  PROP_CACHE_DIR
};

typedef struct _ChamplainFileCachePrivate ChamplainFileCachePrivate;

struct _ChamplainFileCachePrivate
{
  guint size_limit;
  gchar *cache_dir;

  gchar *real_cache_dir;
  sqlite3 *db;
  sqlite3_stmt *stmt_select;
  sqlite3_stmt *stmt_update;
};

static void finalize_sql (ChamplainFileCache *file_cache);
static void delete_temp_cache (ChamplainFileCache *file_cache);
static void init_cache  (ChamplainFileCache *file_cache);
static gchar *get_filename (ChamplainFileCache *file_cache, ChamplainTile *tile);
static gboolean tile_is_expired (ChamplainFileCache *file_cache, ChamplainTile *tile);
static void delete_tile (ChamplainFileCache *file_cache, const gchar *filename);
static void delete_dir_recursive (GFile *parent);

static void fill_tile (ChamplainMapSource *map_source,
                       ChamplainTile *tile);

static void store_tile (ChamplainTileCache *tile_cache,
                        ChamplainTile *tile,
                        const gchar *contents,
                        gsize size);
static void refresh_tile_time (ChamplainTileCache *tile_cache, ChamplainTile *tile);
static void on_tile_filled (ChamplainTileCache *tile_cache, ChamplainTile *tile);
static void clean (ChamplainTileCache *tile_cache);

static void
champlain_file_cache_get_property (GObject *object,
                                   guint property_id,
                                   GValue *value,
                                   GParamSpec *pspec)
{
  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE (object);

  switch (property_id)
    {
    case PROP_SIZE_LIMIT:
      g_value_set_uint (value, champlain_file_cache_get_size_limit (file_cache));
      break;
    case PROP_CACHE_DIR:
      g_value_set_string (value, champlain_file_cache_get_cache_dir (file_cache));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
champlain_file_cache_set_property (GObject *object,
                                   guint property_id,
                                   const GValue *value,
                                   GParamSpec *pspec)
{
  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE (object);
  ChamplainFileCachePrivate *priv = GET_PRIVATE (object);

  switch (property_id)
    {
    case PROP_SIZE_LIMIT:
      champlain_file_cache_set_size_limit (file_cache, g_value_get_uint (value));
      break;
    case PROP_CACHE_DIR:
      priv->cache_dir = g_strdup (g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
champlain_file_cache_dispose (GObject *object)
{
  G_OBJECT_CLASS (champlain_file_cache_parent_class)->dispose (object);
}

static void
finalize_sql (ChamplainFileCache *file_cache)
{
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);
  gint error;

  if (priv->stmt_select)
    {
      sqlite3_finalize (priv->stmt_select);
      priv->stmt_select = NULL;
    }

  if (priv->stmt_update)
    {
      sqlite3_finalize (priv->stmt_update);
      priv->stmt_update = NULL;
    }

  if (priv->db)
    {
      error = sqlite3_close (priv->db);
      if (error != SQLITE_OK)
        DEBUG ("Sqlite returned error %d when closing cache.db", error);
      priv->db = NULL;
    }
}

static void
delete_temp_cache (ChamplainFileCache *file_cache)
{
  ChamplainTileCache *tile_cache = CHAMPLAIN_TILE_CACHE(file_cache);
  ChamplainFileCachePrivate *priv = GET_PRIVATE(file_cache);

  if (!champlain_tile_cache_get_persistent (tile_cache) && priv->real_cache_dir)
    {
      GFile *file = NULL;

      /* delete the directory contents */
      file = g_file_new_for_path (priv->real_cache_dir);
      delete_dir_recursive (file);

      /* delete the directory itself */
      if (!g_file_delete (file, NULL, NULL))
        g_warning ("Failed to remove temporary cache main directory");

      g_object_unref (file);
    }
}

static void
champlain_file_cache_finalize (GObject *object)
{
  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE(object);
  ChamplainTileCache *tile_cache = CHAMPLAIN_TILE_CACHE(file_cache);
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);

  finalize_sql (file_cache);

  if (!champlain_tile_cache_get_persistent (tile_cache))
    delete_temp_cache (file_cache);

  g_free (priv->real_cache_dir);
  g_free (priv->cache_dir);

  G_OBJECT_CLASS (champlain_file_cache_parent_class)->finalize (object);
}

static void
init_cache  (ChamplainFileCache *file_cache)
{
  ChamplainTileCache *tile_cache = CHAMPLAIN_TILE_CACHE(file_cache);
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);
  gchar *filename = NULL;
  gchar *error_msg = NULL;
  gint error;

  g_print ("init! '%d'\n", champlain_tile_cache_get_persistent (tile_cache));
  /* If needed, create the cache's dirs */
  if (priv->cache_dir)
    {
      if (g_mkdir_with_parents (priv->cache_dir, 0700) == -1 && errno != EEXIST)
        {
          g_warning ("Unable to create the image cache path '%s': %s",
                     priv->cache_dir, g_strerror (errno));
          return;
        }
    }

  if (champlain_tile_cache_get_persistent (tile_cache))
    priv->real_cache_dir = g_strdup (priv->cache_dir);
  else
    {
      /* Create temporary directory for non-persistent caches */
      gchar *tmplate = NULL;

      if (priv->cache_dir)
        tmplate = g_build_filename (priv->cache_dir, "champlain-XXXXXX", NULL);
      else
        tmplate = g_build_filename (g_get_tmp_dir (), "champlain-XXXXXX", NULL);

      priv->real_cache_dir = mkdtemp (tmplate);

      if (!priv->real_cache_dir)
        {
          g_warning ("Filed to create filename for temporary cache");
          g_free (tmplate);
        }
    }

  g_return_if_fail (priv->real_cache_dir);

  filename = g_build_filename (priv->real_cache_dir,
                               "cache.db", NULL);
  error = sqlite3_open_v2 (filename, &priv->db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  g_free (filename);

  if (error == SQLITE_ERROR)
    {
      DEBUG ("Sqlite returned error %d when opening cache.db", error);
      return;
    }

  sqlite3_exec (priv->db,
                "PRAGMA synchronous=OFF;"
                "PRAGMA count_changes=OFF;",
                NULL, NULL, &error_msg);
  if (error_msg != NULL)
    {
      DEBUG ("Set PRAGMA: %s", error_msg);
      sqlite3_free (error_msg);
      return;
    }

  sqlite3_exec (priv->db,
                "CREATE TABLE IF NOT EXISTS tiles ("
                "filename TEXT PRIMARY KEY, "
                "etag TEXT, "
                "popularity INT DEFAULT 1, "
                "size INT DEFAULT 0)",
                NULL, NULL, &error_msg);
  if (error_msg != NULL)
    {
      DEBUG ("Creating table 'tiles' failed: %s", error_msg);
      sqlite3_free (error_msg);
      return;
    }

  error = sqlite3_prepare_v2 (priv->db,
                              "SELECT etag FROM tiles WHERE filename = ?", -1,
                              &priv->stmt_select, NULL);
  if (error != SQLITE_OK)
    {
      priv->stmt_select = NULL;
      DEBUG ("Failed to prepare the select Etag statement, error:%d: %s",
             error, sqlite3_errmsg (priv->db));
      return;
    }

  error = sqlite3_prepare_v2 (priv->db,
                              "UPDATE tiles SET popularity = popularity + 1 WHERE filename = ?", -1,
                              &priv->stmt_update, NULL);
  if (error != SQLITE_OK)
    {
      priv->stmt_update = NULL;
      DEBUG ("Failed to prepare the update popularity statement, error: %s",
             sqlite3_errmsg (priv->db));
      return;
    }

  g_object_notify (G_OBJECT (file_cache), "cache-dir");
}

static void
champlain_file_cache_class_init (ChamplainFileCacheClass *klass)
{
  ChamplainMapSourceClass *map_source_class = CHAMPLAIN_MAP_SOURCE_CLASS (klass);
  ChamplainTileCacheClass *tile_cache_class = CHAMPLAIN_TILE_CACHE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  g_type_class_add_private (klass, sizeof (ChamplainFileCachePrivate));

  object_class->finalize = champlain_file_cache_finalize;
  object_class->dispose = champlain_file_cache_dispose;
  object_class->get_property = champlain_file_cache_get_property;
  object_class->set_property = champlain_file_cache_set_property;

  /**
  * ChamplainFileCache:size-limit:
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
                             100000000,
                             G_PARAM_CONSTRUCT | G_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_SIZE_LIMIT, pspec);

  /**
  * ChamplainFileCache:cache-dir:
  *
  * The directory where the tile database is stored.
  *
  * Since: 0.6
  */
  pspec = g_param_spec_string ("cache-dir",
                               "Cache Directory",
                               "The directory of the cache",
                               "",
                               G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_CACHE_DIR, pspec);

  tile_cache_class->store_tile = store_tile;
  tile_cache_class->refresh_tile_time = refresh_tile_time;
  tile_cache_class->on_tile_filled = on_tile_filled;
  tile_cache_class->clean = clean;

  map_source_class->fill_tile = fill_tile;
}

static void
champlain_file_cache_init (ChamplainFileCache *file_cache)
{
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);

  priv->size_limit = 100000000;
#ifdef USE_MAEMO
  priv->cache_dir = g_strdup ("/home/user/MyDocs/.Maps/");
#else
  priv->cache_dir = g_build_path (G_DIR_SEPARATOR_S, g_get_user_cache_dir (), "champlain", NULL);
#endif

  priv->real_cache_dir = NULL;
  priv->db = NULL;
  priv->stmt_select = NULL;
  priv->stmt_update = NULL;

  init_cache (file_cache);
}

/**
 * champlain_file_cache_new:
 *
 * Default constructor of #ChamplainFileCache.
 *
 * Returns: a constructed permanent cache of maximal size 100000000 B inside
 * ~/.cache/champlain.
 *
 * Since: 0.6
 */
ChamplainFileCache* champlain_file_cache_new (void)
{
  return CHAMPLAIN_FILE_CACHE (g_object_new (CHAMPLAIN_TYPE_FILE_CACHE, NULL));
}

/**
 * champlain_file_cache_new_full:
 * @size_limit: maximal size of the cache in bytes
 * @cache_dir: the directory where the cache is created. For temporary caches
 * one more directory with random name is created inside this directory.
 * When cache_dir == NULL, a cache in ~/.cache/champlain is used for permanent
 * caches and /tmp for temporary caches.
 * @persistent: if TRUE, the cache is persistent; otherwise the cache is
 * temporary and will be deleted when the cache object is destructed.
 *
 * Constructor of #ChamplainFileCache.
 *
 * Returns: a constructed #ChamplainFileCache
 *
 * Since: 0.6
 */
ChamplainFileCache* champlain_file_cache_new_full (guint size_limit,
    const gchar *cache_dir, gboolean persistent)
{
  ChamplainFileCache * cache;
  cache = g_object_new (CHAMPLAIN_TYPE_FILE_CACHE, "size-limit", size_limit,
                        "cache-dir", cache_dir, "persistent-cache", persistent, NULL);
  return cache;
}

/**
 * champlain_file_cache_get_size_limit:
 * @file_cache: a #ChamplainCache
 *
 * Gets the cache size limit in bytes.
 *
 * Returns: size limit
 *
 * Since: 0.4
 */
guint
champlain_file_cache_get_size_limit (ChamplainFileCache *file_cache)
{
  g_return_val_if_fail (CHAMPLAIN_IS_FILE_CACHE (file_cache), 0);

  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);
  return priv->size_limit;
}

/**
 * champlain_file_cache_get_cache_dir:
 * @file_cache: a #ChamplainCache
 *
 * Gets the directory where the cache database is stored.
 *
 * Returns: the directory
 *
 * Since: 0.6
 */
const gchar *
champlain_file_cache_get_cache_dir (ChamplainFileCache *file_cache)
{
  g_return_val_if_fail (CHAMPLAIN_IS_FILE_CACHE (file_cache), NULL);

  ChamplainFileCachePrivate *priv = GET_PRIVATE(file_cache);
  return priv->cache_dir;
}

/**
 * champlain_file_cache_set_size_limit:
 * @file_cache: a #ChamplainCache
 * @size_limit: the cache limit in bytes
 *
 * Sets the cache size limit in bytes.
 *
 * Since: 0.4
 */
void
champlain_file_cache_set_size_limit (ChamplainFileCache *file_cache,
                                     guint size_limit)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (file_cache));

  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);

  priv->size_limit = size_limit;
  g_object_notify (G_OBJECT (file_cache), "size-limit");
}

static gchar *
get_filename (ChamplainFileCache *file_cache, ChamplainTile *tile)
{
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);

  g_return_val_if_fail (CHAMPLAIN_IS_FILE_CACHE (file_cache), NULL);
  g_return_val_if_fail (CHAMPLAIN_IS_TILE (tile), NULL);
  g_return_val_if_fail (priv->real_cache_dir, NULL);

  ChamplainMapSource* map_source = CHAMPLAIN_MAP_SOURCE(file_cache);

  gchar *filename = g_strdup_printf ("%s" G_DIR_SEPARATOR_S
                                     "%s" G_DIR_SEPARATOR_S
                                     "%d" G_DIR_SEPARATOR_S
                                     "%d" G_DIR_SEPARATOR_S "%d.png",
                                     priv->real_cache_dir,
                                     champlain_map_source_get_id (map_source),
                                     champlain_tile_get_zoom_level (tile),
                                     champlain_tile_get_x (tile),
                                     champlain_tile_get_y (tile));
  return filename;
}

static gboolean
tile_is_expired (ChamplainFileCache *file_cache, ChamplainTile *tile)
{
  g_return_val_if_fail (CHAMPLAIN_FILE_CACHE (file_cache), FALSE);
  g_return_val_if_fail (CHAMPLAIN_TILE (tile), FALSE);

  GTimeVal now = {0, };
  const GTimeVal *modified_time = champlain_tile_get_modified_time (tile);
  gboolean validate_cache = TRUE;

  if (modified_time)
    {
      g_get_current_time (&now);
      g_time_val_add (&now, (-24ul * 60ul * 60ul * 1000ul * 1000ul * 7ul)); // Cache expires in 7 days
      validate_cache = modified_time->tv_sec < now.tv_sec;
    }

  DEBUG ("%p is %s expired", tile, (validate_cache ? "": "not"));

  return validate_cache;
}

static void
fill_tile (ChamplainMapSource *map_source,
           ChamplainTile *tile)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (map_source));
  g_return_if_fail (CHAMPLAIN_IS_TILE (tile));

  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE(map_source);
  ChamplainMapSource *next_source = champlain_map_source_get_next_source (map_source);
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);
  GFileInfo *info = NULL;
  GFile *file = NULL;
  GError *gerror = NULL;
  ClutterActor *actor = NULL;
  gchar *filename = NULL;
  GTimeVal modified_time = {0,};

  if (champlain_tile_get_content (tile))
    /* Previous cache in the chain is validating its contents */
    goto load_next;

  filename = get_filename (file_cache, tile);
  DEBUG ("fill of %s", filename);

  /* Load the cached version */
  actor = clutter_texture_new ();
  clutter_texture_set_load_async (CLUTTER_TEXTURE (actor), TRUE);
  clutter_texture_set_from_file (CLUTTER_TEXTURE (actor), filename, &gerror);
  if (actor)
    {
      champlain_tile_set_content (tile, actor, FALSE);
      champlain_tile_set_size (tile, champlain_map_source_get_tile_size (map_source));
    }
  else
    {
      DEBUG ("Failed to load tile %s, error: %s",
             filename, gerror->message);
      g_error_free (gerror);
      goto load_next;
    }

  /* Retrieve modification time */
  file = g_file_new_for_path (filename);
  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info)
    {
      g_file_info_get_modification_time (info, &modified_time);
      champlain_tile_set_modified_time (tile, &modified_time);

      g_object_unref (info);
    }
  g_object_unref (file);

  /* Notify other caches that the tile has been filled */
  if (CHAMPLAIN_IS_TILE_CACHE(next_source))
    {
      on_tile_filled (CHAMPLAIN_TILE_CACHE(next_source), tile);
    }

  if (tile_is_expired (file_cache, tile))
    {
      int sql_rc = SQLITE_OK;

      /* Retrieve etag */
      sql_rc = sqlite3_bind_text (priv->stmt_select, 1, filename, -1, SQLITE_STATIC);
      if (sql_rc == SQLITE_ERROR)
        {
          DEBUG ("Failed to prepare the SQL query for finding the Etag of '%s', error: %s",
                 filename, sqlite3_errmsg (priv->db));
          goto load_next;
        }

      sql_rc = sqlite3_step (priv->stmt_select);
      if (sql_rc == SQLITE_ROW)
        {
          const gchar *etag = (const gchar *) sqlite3_column_text (priv->stmt_select, 0);
          champlain_tile_set_etag (CHAMPLAIN_TILE (tile), etag);
        }
      else if (sql_rc == SQLITE_DONE)
        {
          DEBUG ("'%s' does't have an etag",
                 filename);
          goto load_next;
        }
      else if (sql_rc == SQLITE_ERROR)
        {
          DEBUG ("Failed to finding the Etag of '%s', %d error: %s",
                 filename, sql_rc, sqlite3_errmsg (priv->db));
          goto load_next;
        }

      /* Validate the tile */
      /* goto load_next; */
    }
  else
    {
      /* Tile loaded and no validation needed - done */
      champlain_tile_set_state (tile, CHAMPLAIN_STATE_DONE);
      goto cleanup;
    }

load_next:
  if (CHAMPLAIN_IS_MAP_SOURCE(next_source))
    champlain_map_source_fill_tile (next_source, tile);

  /* if we have some content, use the tile even if it wasn't validated */
  if (champlain_tile_get_content (tile) &&
      champlain_tile_get_state (tile) != CHAMPLAIN_STATE_DONE)
    champlain_tile_set_state (tile, CHAMPLAIN_STATE_DONE);

cleanup:
  sqlite3_reset (priv->stmt_select);
  g_free (filename);
}

static void
refresh_tile_time (ChamplainTileCache *tile_cache, ChamplainTile *tile)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (tile_cache));

  ChamplainMapSource *map_source = CHAMPLAIN_MAP_SOURCE(tile_cache);
  ChamplainMapSource *next_source = champlain_map_source_get_next_source (map_source);
  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE(tile_cache);
  gchar *filename = NULL;
  GFile *file;
  GFileInfo *info;

  filename = get_filename (file_cache, tile);
  file = g_file_new_for_path (filename);
  g_free (filename);

  info = g_file_query_info (file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);

  if (info)
    {
      GTimeVal now = {0, };

      g_get_current_time (&now);

      g_file_info_set_modification_time (info, &now);
      g_file_set_attributes_from_info (file, info, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    }

  g_object_unref (file);
  g_object_unref (info);

  if (CHAMPLAIN_IS_TILE_CACHE(next_source))
    {
      refresh_tile_time (CHAMPLAIN_TILE_CACHE(next_source), tile);
    }
}

static void
store_tile (ChamplainTileCache *tile_cache,
            ChamplainTile *tile,
            const gchar *contents,
            gsize size)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (tile_cache));

  ChamplainMapSource *map_source = CHAMPLAIN_MAP_SOURCE(tile_cache);
  ChamplainMapSource *next_source = champlain_map_source_get_next_source (map_source);
  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE(tile_cache);
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);
  gchar *query = NULL;
  gchar *error = NULL;
  gchar *path = NULL;
  gchar *filename = NULL;
  GError *gerror = NULL;
  GFile *file;
  GFileOutputStream *ostream;
  gsize bytes_written;

  DEBUG ("Update of %p", tile);

  filename = get_filename (file_cache, tile);
  file = g_file_new_for_path (filename);

  /* If the file exists, delete it */
  g_file_delete (file, NULL, NULL);

  /* If needed, create the cache's dirs */
  path = g_path_get_dirname (filename);
  if (g_mkdir_with_parents (path, 0700) == -1)
    {
      if (errno != EEXIST)
        {
          g_warning ("Unable to create the image cache path '%s': %s",
                     path, g_strerror (errno));
          goto store_next;
        }
    }

  ostream = g_file_create (file, G_FILE_CREATE_PRIVATE, NULL, &gerror);
  if (!ostream)
    {
      DEBUG ("GFileOutputStream creation failed: %s", gerror->message);
      g_error_free (gerror);
      goto store_next;
    }

  /* Write the cache */
  if (!g_output_stream_write_all (G_OUTPUT_STREAM(ostream), contents, size, &bytes_written, NULL, &gerror))
    {
      DEBUG ("Writing file contents failed: %s", gerror->message);
      g_error_free (gerror);
      g_object_unref (ostream);
      goto store_next;
    }

  g_object_unref (ostream);

  query = sqlite3_mprintf ("REPLACE INTO tiles (filename, etag, size) VALUES (%Q, %Q, %d)",
                           filename,
                           champlain_tile_get_etag (tile),
                           size);
  sqlite3_exec (priv->db, query, NULL, NULL, &error);
  if (error != NULL)
    {
      DEBUG ("Saving Etag and size failed: %s", error);
      sqlite3_free (error);
    }
  sqlite3_free (query);

store_next:
  if (CHAMPLAIN_IS_TILE_CACHE(next_source))
    {
      store_tile (CHAMPLAIN_TILE_CACHE(next_source), tile, contents, size);
    }

  g_free (filename);
  g_free (path);
  g_object_unref (file);
}

static void
on_tile_filled (ChamplainTileCache *tile_cache, ChamplainTile *tile)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (tile_cache));
  g_return_if_fail (CHAMPLAIN_IS_TILE (tile));

  ChamplainMapSource *map_source = CHAMPLAIN_MAP_SOURCE(tile_cache);
  ChamplainMapSource *next_source = champlain_map_source_get_next_source (map_source);
  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE(tile_cache);
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);
  int sql_rc = SQLITE_OK;
  gchar *filename = NULL;

  filename = get_filename (file_cache, tile);

  DEBUG ("popularity of %s", filename);

  sql_rc = sqlite3_bind_text (priv->stmt_update, 1, filename, -1, SQLITE_STATIC);
  if (sql_rc != SQLITE_OK)
    {
      DEBUG ("Failed to set values to the popularity query of '%s', error: %s",
             filename, sqlite3_errmsg (priv->db));
      goto call_next;
    }

  sql_rc = sqlite3_step (priv->stmt_update);
  if (sql_rc != SQLITE_DONE)
    {
      /* may not be present in this cache */
      goto call_next;
    }

call_next:
  if (CHAMPLAIN_IS_TILE_CACHE(next_source))
    {
      on_tile_filled (CHAMPLAIN_TILE_CACHE(next_source), tile);
    }
}

static void
clean (ChamplainTileCache *tile_cache)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (tile_cache));

  ChamplainFileCache *file_cache = CHAMPLAIN_FILE_CACHE(tile_cache);
  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);

  g_return_if_fail (!champlain_tile_cache_get_persistent (tile_cache));

  finalize_sql (file_cache);
  delete_temp_cache (file_cache);

  g_free (priv->real_cache_dir);
  priv->real_cache_dir = NULL;

  init_cache (file_cache);
}

static void
delete_tile (ChamplainFileCache *file_cache, const gchar *filename)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (file_cache));
  gchar *query, *error = NULL;
  GError *gerror = NULL;
  GFile *file;

  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);

  query = sqlite3_mprintf ("DELETE FROM tiles WHERE filename = %Q", filename);
  sqlite3_exec (priv->db, query, NULL, NULL, &error);
  if (error != NULL)
    {
      DEBUG ("Deleting tile from db failed: %s", error);
      sqlite3_free (error);
    }
  sqlite3_free (query);

  file = g_file_new_for_path (filename);
  if (!g_file_delete (file, NULL, &gerror))
    {
      DEBUG ("Deleting tile from disk failed: %s", gerror->message);
      g_error_free (gerror);
    }
  g_object_unref (file);
}

static gboolean
purge_on_idle (gpointer data)
{
  champlain_file_cache_purge (CHAMPLAIN_FILE_CACHE (data));
  return FALSE;
}

/**
 * champlain_file_cache_purge_on_idle:
 * @file_cache: a #ChamplainCache
 *
 * Purge the cache from the less popular tiles until cache's size limit is reached.
 * This is a non blocking call as the purge will happen when the application is idle
 *
 * Since: 0.4
 */
void
champlain_file_cache_purge_on_idle (ChamplainFileCache *file_cache)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (file_cache));
  g_idle_add (purge_on_idle, file_cache);
}

/**
 * champlain_file_cache_purge:
 * @file_cache: a #ChamplainCache
 *
 * Purge the cache from the less popular tiles until cache's size limit is reached.
 *
 * Since: 0.4
 */
void
champlain_file_cache_purge (ChamplainFileCache *file_cache)
{
  g_return_if_fail (CHAMPLAIN_IS_FILE_CACHE (file_cache));

  ChamplainFileCachePrivate *priv = GET_PRIVATE (file_cache);
  gchar *query;
  sqlite3_stmt *stmt;
  int rc = 0;
  guint current_size = 0;
  guint highest_popularity = 0;
  gchar *error;

  query = "SELECT SUM (size) FROM tiles";
  rc = sqlite3_prepare (priv->db, query, strlen (query), &stmt, NULL);
  if (rc != SQLITE_OK)
    {
      DEBUG ("Can't compute cache size %s", sqlite3_errmsg (priv->db));
    }

  rc = sqlite3_step (stmt);
  if (rc != SQLITE_ROW)
    {
      DEBUG ("Failed to count the total cache consumption %s",
             sqlite3_errmsg (priv->db));
      sqlite3_finalize (stmt);
      return;
    }

  current_size = sqlite3_column_int (stmt, 0);
  if (current_size < priv->size_limit)
    {
      DEBUG ("Cache doesn't need to be purged at %d bytes", current_size);
      sqlite3_finalize (stmt);
      return;
    }

  sqlite3_finalize (stmt);

  /* Ok, delete the less popular tiles until size_limit reached */
  query = "SELECT filename, size, popularity FROM tiles ORDER BY popularity";
  rc = sqlite3_prepare (priv->db, query, strlen (query), &stmt, NULL);
  if (rc != SQLITE_OK)
    {
      DEBUG ("Can't fetch tiles to delete: %s", sqlite3_errmsg (priv->db));
    }

  rc = sqlite3_step (stmt);
  while (rc == SQLITE_ROW && current_size > priv->size_limit)
    {
      const char *filename = sqlite3_column_text (stmt, 0);
      guint size;

      filename = sqlite3_column_text (stmt, 0);
      size = sqlite3_column_int (stmt, 1);
      highest_popularity = sqlite3_column_int (stmt, 2);
      DEBUG ("Deleting %s of size %d", filename, size);

      delete_tile (file_cache, filename);

      current_size -= size;

      rc = sqlite3_step (stmt);
    }
  DEBUG ("Cache size is now %d", current_size);

  sqlite3_finalize (stmt);

  query = sqlite3_mprintf ("UPDATE tiles SET popularity = popularity - %d",
                           highest_popularity);
  sqlite3_exec (priv->db, query, NULL, NULL, &error);
  if (error != NULL)
    {
      DEBUG ("Updating popularity failed: %s", error);
      sqlite3_free (error);
    }
  sqlite3_free (query);
}

static void
delete_dir_recursive (GFile *parent)
{
  g_return_if_fail (parent);

  GError *error = NULL;
  GFileEnumerator *enumerator;
  GFileInfo *info;
  GFile *child;

  enumerator = g_file_enumerate_children (parent, "*",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);

  if (!enumerator)
    {
      DEBUG ("Failed to create file enumerator in delete_dir_recursive: %s", error->message);
      g_error_free (error);
      return;
    }

  info = g_file_enumerator_next_file (enumerator, NULL, NULL);
  while (info && !error)
    {
      child = g_file_get_child (parent, g_file_info_get_name (info));

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        delete_dir_recursive (child);

      error = NULL;
      if (!g_file_delete (child, NULL, &error))
        {
          DEBUG ("Deleting tile from disk failed: %s", error->message);
          g_error_free (error);
        }

      g_object_unref (child);
      g_object_unref (info);
      info = g_file_enumerator_next_file (enumerator, NULL, NULL);
    }

  g_file_enumerator_close (enumerator, NULL, NULL);
}