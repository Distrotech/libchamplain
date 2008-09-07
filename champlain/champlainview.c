/*
 * Copyright (C) 2008 Pierre-Luc Beaudoin <pierre-luc@squidy.info>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include "champlain_defines.h"
#include "champlain_private.h"
#include "champlain.h"
#include "champlain-marshal.h"
#include "champlainview.h"
#include "map.h"
#include "tile.h"
#include "zoomlevel.h"

#include <clutter/clutter.h>
#include <glib.h>
#include <glib-object.h>
#include <gtk-clutter-embed.h>
#include <math.h>
#include <tidy-adjustment.h>
#include <tidy-finger-scroll.h>
#include <tidy-scrollable.h>
#include <tidy-viewport.h>

enum
{
  /* normal signals */
  SIGNAL_TBD,
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_LONGITUDE,
  PROP_LATITUDE,
  PROP_ZOOM_LEVEL,
  PROP_MAP_SOURCE,
  PROP_OFFLINE,
  PROP_DECEL_RATE,
};

static guint champlain_view_signals[LAST_SIGNAL] = { 0, };

#define CHAMPLAIN_VIEW_GET_PRIVATE(obj)    (G_TYPE_INSTANCE_GET_PRIVATE((obj), CHAMPLAIN_TYPE_VIEW, ChamplainViewPrivate))

struct _ChamplainViewPrivate
{
  GtkWidget *clutter_embed;

  ChamplainMapSource map_source;
  gint zoom_level; // only used when the zoom-level property is set before map is created

  ClutterActor *map_layer;
  ClutterActor *viewport;
  ClutterActor *finger_scroll;
  GdkRectangle viewport_size;

  ClutterActor *user_layers;

  Map *map;

  GdkCursor *cursor_hand_open;
  GdkCursor *cursor_hand_closed;

  gboolean offline;
};

G_DEFINE_TYPE (ChamplainView, champlain_view, GTK_TYPE_ALIGNMENT);

static gdouble viewport_get_current_longitude(ChamplainViewPrivate *priv);
static gdouble viewport_get_current_latitude(ChamplainViewPrivate *priv);
static gdouble viewport_get_longitude_at(ChamplainViewPrivate *priv, gint x);
static gdouble viewport_get_latitude_at(ChamplainViewPrivate *priv, gint y);
static gboolean scroll_event (ClutterActor *actor, ClutterScrollEvent *event, ChamplainView *view);
static void marker_reposition_cb (ChamplainMarker *marker, ChamplainView *view);
static void layer_reposition_cb (ClutterActor *layer, ChamplainView *view);
static void marker_reposition (ChamplainView *view);
static void create_initial_map(ChamplainView *view);
static void resize_viewport(ChamplainView *view);
static void champlain_view_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void champlain_view_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void champlain_view_finalize (GObject *object);
static void champlain_view_class_init (ChamplainViewClass *champlainViewClass);
static void champlain_view_init (ChamplainView *view);
static void viewport_x_changed_cb(GObject *gobject, GParamSpec *arg1, ChamplainView *view);
static void view_size_allocated_cb (GtkWidget *widget, GtkAllocation *allocation, ChamplainView *view);
static void notify_marker_reposition_cb(ChamplainMarker *marker, GParamSpec *arg1, ChamplainView *view);
static void layer_add_marker_cb (ClutterGroup *layer, ChamplainMarker *marker, ChamplainView *view);
static void connect_marker_notify_cb (ChamplainMarker *marker, ChamplainView *view);

static gdouble
viewport_get_longitude_at(ChamplainViewPrivate *priv, gint x)
{
  return priv->map->x_to_longitude(priv->map, x, priv->map->current_level->level);
}

static gdouble
viewport_get_current_longitude(ChamplainViewPrivate *priv)
{
  return viewport_get_longitude_at(priv,
    priv->map->current_level->anchor.x + priv->viewport_size.x + priv->viewport_size.width / 2.0);
}

static gdouble
viewport_get_latitude_at(ChamplainViewPrivate *priv, gint y)
{
  return priv->map->y_to_latitude(priv->map, y, priv->map->current_level->level);
}

static gdouble
viewport_get_current_latitude(ChamplainViewPrivate *priv)
{
  return viewport_get_latitude_at(priv,
    priv->map->current_level->anchor.y + priv->viewport_size.y + priv->viewport_size.height / 2.0);
}

static gboolean
scroll_event (ClutterActor *actor, ClutterScrollEvent *event, ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  ClutterActor *group = priv->map->current_level->group;
  gboolean success = FALSE;

  // Keep the lon, lat where the mouse is
  gdouble lon = viewport_get_longitude_at(priv,
    priv->viewport_size.x + event->x + priv->map->current_level->anchor.x);
  gdouble lat = viewport_get_latitude_at(priv, 
    priv->viewport_size.y + event->y + priv->map->current_level->anchor.y);

  // How far was it from the center of the viewport (in px)
  gint x_diff = priv->viewport_size.width / 2 - event->x;
  gint y_diff = priv->viewport_size.height / 2 - event->y;

  if (event->direction == CLUTTER_SCROLL_UP)
    success = map_zoom_in(priv->map);
  else if (event->direction == CLUTTER_SCROLL_DOWN) 
    success = map_zoom_out(priv->map);

  if (success)
    {
      // Get the new x,y in the new zoom level
      gint x2 = priv->map->longitude_to_x(priv->map, lon, priv->map->current_level->level);
      gint y2 = priv->map->latitude_to_y(priv->map, lat, priv->map->current_level->level);
      // Get the new lon,lat of these new x,y minus the distance from the viewport center
      gdouble lon2 = priv->map->x_to_longitude(priv->map, x2 + x_diff, priv->map->current_level->level);
      gdouble lat2 = priv->map->y_to_latitude(priv->map, y2 + y_diff, priv->map->current_level->level);

      resize_viewport(view);
      clutter_container_remove_actor (CLUTTER_CONTAINER (priv->map_layer), group);
      clutter_container_add_actor (CLUTTER_CONTAINER (priv->map_layer), priv->map->current_level->group);
      champlain_view_center_on(view, lon2, lat2);
      marker_reposition(view);

      g_object_notify(G_OBJECT(view), "zoom-level");
    }

}

static void
marker_reposition_cb (ChamplainMarker *marker, ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  ChamplainMarkerPrivate *marker_priv = CHAMPLAIN_MARKER_GET_PRIVATE (marker);

  gint x, y;

  if(priv->map)
    {
      x = priv->map->longitude_to_x(priv->map, marker_priv->lon, priv->map->current_level->level);
      y = priv->map->latitude_to_y(priv->map, marker_priv->lat, priv->map->current_level->level);

      clutter_actor_set_position(CLUTTER_ACTOR(marker),
        x - marker_priv->anchor.x - priv->map->current_level->anchor.x,
        y - marker_priv->anchor.y - priv->map->current_level->anchor.y);
    }
}

static void
notify_marker_reposition_cb(ChamplainMarker *marker, GParamSpec *arg1, ChamplainView *view)
{
  marker_reposition_cb(marker, view);
}

static void
layer_add_marker_cb (ClutterGroup *layer, ChamplainMarker *marker, ChamplainView *view)
{
  g_signal_connect (marker,
                    "notify::longitude",
                    G_CALLBACK (notify_marker_reposition_cb),
                    view);
}

static void
connect_marker_notify_cb (ChamplainMarker *marker, ChamplainView *view)
{
  g_signal_connect (marker,
                    "notify::longitude",
                    G_CALLBACK (notify_marker_reposition_cb),
                    view);
}

static void
layer_reposition_cb (ClutterActor *layer, ChamplainView *view)
{
  clutter_container_foreach(CLUTTER_CONTAINER(layer), CLUTTER_CALLBACK(marker_reposition_cb), view);
}

static void
marker_reposition (ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  clutter_container_foreach(CLUTTER_CONTAINER(priv->user_layers), CLUTTER_CALLBACK(layer_reposition_cb), view);
}

static void
create_initial_map(ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  priv->map = map_new(priv->map_source);
  map_load_level(priv->map, priv->zoom_level);
  clutter_container_add_actor (CLUTTER_CONTAINER (priv->map_layer), priv->map->current_level->group);

  marker_reposition(view);

  g_object_notify(G_OBJECT(view), "zoom-level");
  g_object_notify(G_OBJECT(view), "map-source");
}

static void
resize_viewport(ChamplainView *view)
{
  gdouble lower, upper;
  TidyAdjustment *hadjust, *vadjust;

  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  if(!priv->map)
    {
      create_initial_map(view);
    }

  clutter_actor_set_size (priv->finger_scroll, priv->viewport_size.width, priv->viewport_size.height);


  tidy_scrollable_get_adjustments (TIDY_SCROLLABLE (priv->viewport), &hadjust, &vadjust);

  if (priv->map->current_level->level < 8)
    {
      lower = -priv->viewport_size.width / 2;
      upper = zoom_level_get_width(priv->map->current_level) - priv->viewport_size.width / 2;
    }
  else
    {
      lower = 0;
      upper = G_MAXINT16;
    }
  g_object_set (hadjust, "lower", lower, "upper", upper,
                "step-increment", 1.0, "elastic", TRUE, NULL);

  if (priv->map->current_level->level < 8)
    {
      lower = -priv->viewport_size.height / 2;
      upper = zoom_level_get_height(priv->map->current_level) - priv->viewport_size.height / 2;
    }
  else
    {
      lower = 0;
      upper = G_MAXINT16;
    }
  g_object_set (vadjust, "lower", lower, "upper", upper,
                "step-increment", 1.0, "elastic", TRUE, NULL);
}

static void
champlain_view_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  ChamplainView *view = CHAMPLAIN_VIEW(object);
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  switch(prop_id)
    {
      case PROP_LONGITUDE:
        g_value_set_double(value, viewport_get_current_longitude(priv));
        break;
      case PROP_LATITUDE:
        g_value_set_double(value, viewport_get_current_latitude(priv));
        break;
      case PROP_ZOOM_LEVEL:
        {
          if (priv->map)
            {
              g_value_set_int(value, priv->map->current_level->level);
            }
          else
            {
              g_value_set_int(value, 0);
            }
          break;
        }
      case PROP_MAP_SOURCE:
        g_value_set_int(value, priv->map_source);
        break;
      case PROP_OFFLINE:
        g_value_set_boolean(value, priv->offline);
        break;
      case PROP_DECEL_RATE:
        {
          gdouble decel;
          g_object_get (priv->finger_scroll, "decel-rate", decel, NULL);
          g_value_set_double(value, decel);
          break;
        }
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
champlain_view_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  ChamplainView *view = CHAMPLAIN_VIEW(object);
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  switch(prop_id)
  {
    case PROP_LONGITUDE:
      {
        gdouble lon = g_value_get_double(value);
        gdouble lat = viewport_get_current_latitude(priv);
        champlain_view_center_on(view, lon, lat);
        break;
      }
    case PROP_LATITUDE:
      {
        gdouble lon = viewport_get_current_longitude(priv);
        gdouble lat = g_value_get_double(value);
        champlain_view_center_on(view, lon, lat);
        break;
      }
    case PROP_ZOOM_LEVEL:
      {
        gint level = g_value_get_int(value);
        if (priv->map)
          {
            if (level != priv->map->current_level->level)
              {
                ClutterActor *group = priv->map->current_level->group;
                gdouble lon = viewport_get_current_longitude(priv);
                gdouble lat = viewport_get_current_latitude(priv);
                if (map_zoom_to(priv->map, level))
                  {
                    resize_viewport(view);
                    clutter_container_remove_actor (CLUTTER_CONTAINER (priv->map_layer), group);
                    clutter_container_add_actor (CLUTTER_CONTAINER (priv->map_layer), priv->map->current_level->group);
                    champlain_view_center_on(view, lon, lat);
                    marker_reposition(view);
                  }
              }
          }
        else
          {
            priv->zoom_level = level;
          }
        break;
      }
    case PROP_MAP_SOURCE:
      {
        ChamplainMapSource source = g_value_get_int(value);
        if (priv->map_source != source)
          {
            priv->map_source = source;
            if (priv->map) {
              gint currentLevel = priv->map->current_level->level;
              GdkPoint anchor = priv->map->current_level->anchor;
              map_free(priv->map);
              priv->map = map_new(priv->map_source);

              // Keep same zoom level
              if (currentLevel > priv->map->zoom_levels)
                currentLevel = priv->map->zoom_levels;

              map_load_level(priv->map, currentLevel);
              priv->map->current_level->anchor = anchor;

              map_load_visible_tiles (priv->map, priv->viewport_size, priv->offline);
              clutter_container_add_actor (CLUTTER_CONTAINER (priv->map_layer), priv->map->current_level->group);

              marker_reposition(view);
            }
          }
        break;
      }
    case PROP_OFFLINE:
      priv->offline = g_value_get_boolean(value);
      break;
    case PROP_DECEL_RATE:
      {
        gdouble decel = g_value_get_double(value);
        g_object_set (priv->finger_scroll, "decel-rate", decel, NULL);
        break;
      }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
champlain_view_finalize (GObject *object)
{
  ChamplainView *view = CHAMPLAIN_VIEW (object);
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  G_OBJECT_CLASS (champlain_view_parent_class)->finalize (object);
}

static void
champlain_view_class_init (ChamplainViewClass *champlainViewClass)
{
  g_type_class_add_private (champlainViewClass, sizeof (ChamplainViewPrivate));

  GObjectClass *object_class = G_OBJECT_CLASS (champlainViewClass);
  object_class->finalize = champlain_view_finalize;
  object_class->get_property = champlain_view_get_property;
  object_class->set_property = champlain_view_set_property;

  /**
  * ChamplainView:longitude:
  *
  * The longitude coordonate of the map
  *
  * Since: 0.1
  */
  g_object_class_install_property(object_class, PROP_LONGITUDE,
                                  g_param_spec_float("longitude",
                                                     "Longitude",
                                                     "The longitude coordonate of the map",
                                                     -180.0f,
                                                     180.0f,
                                                     0.0f,
                                                     CHAMPLAIN_PARAM_READWRITE));

  /**
  * ChamplainView:latitude:
  *
  * The latitude coordonate of the map
  *
  * Since: 0.1
  */
  g_object_class_install_property(object_class, PROP_LATITUDE,
                                  g_param_spec_float("latitude",
                                                     "Latitude",
                                                     "The latitude coordonate of the map",
                                                     -90.0f,
                                                     90.0f,
                                                     0.0f,
                                                     CHAMPLAIN_PARAM_READWRITE));

  /**
  * ChamplainView:zoom-level:
  *
  * The level of zoom of the content.
  *
  * Since: 0.1
  */
  g_object_class_install_property(object_class, PROP_ZOOM_LEVEL,
                                  g_param_spec_int("zoom-level",
                                                   "Zoom level",
                                                   "The level of zoom of the map",
                                                   0,
                                                   20,
                                                   3,
                                                   CHAMPLAIN_PARAM_READWRITE));


  /**
  * ChamplainView:map-source:
  *
  * The #ChamplainMapSource being displayed
  *
  * Since: 0.2
  */
  g_object_class_install_property(object_class, PROP_MAP_SOURCE,
                                  g_param_spec_int("map-source",
                                                   "Map source",
                                                   "The map source being displayed",
                                                   0,
                                                   CHAMPLAIN_MAP_SOURCE_COUNT,
                                                   CHAMPLAIN_MAP_SOURCE_OPENSTREETMAP,
                                                   CHAMPLAIN_PARAM_READWRITE));

  /**
  * ChamplainView:offline:
  *
  * If true, will fetch tiles from the Internet, otherwise, will only use cached content.
  *
  * Since: 0.2
  */
  g_object_class_install_property(object_class, PROP_OFFLINE,
                                  g_param_spec_boolean("offline",
                                                       "Offline Mode",
                                                       "If viewer is in offline mode.",
                                                       FALSE,
                                                       CHAMPLAIN_PARAM_READWRITE));

  /**
  * ChamplainView:decel-rate:
  *
  * The deceleration rate for the kinetic mode.
  *
  * Since: 0.2
  */
  g_object_class_install_property (object_class,
                                   PROP_DECEL_RATE,
                                   g_param_spec_double ("decel-rate",
                                                        "Deceleration rate",
                                                        "Rate at which the view "
                                                        "will decelerate in "
                                                        "kinetic mode.",
                                                        1.0,
                                                        2.0,
                                                        1.1,
                                                        G_PARAM_READWRITE));
}

static void
champlain_view_init (ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  priv->map_source = CHAMPLAIN_MAP_SOURCE_OPENSTREETMAP;
  priv->zoom_level = 3;
  priv->offline = FALSE;
}

static void
viewport_x_changed_cb(GObject *gobject, GParamSpec *arg1, ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  GdkPoint rect;
  tidy_viewport_get_origin(TIDY_VIEWPORT(priv->viewport), &rect.x, &rect.y, NULL);

  if (rect.x == priv->viewport_size.x &&
      rect.y == priv->viewport_size.y)
      return;

  priv->viewport_size.x = rect.x;
  priv->viewport_size.y = rect.y;

  map_load_visible_tiles (priv->map, priv->viewport_size, priv->offline);

  g_object_notify(G_OBJECT(view), "longitude");
  g_object_notify(G_OBJECT(view), "latitude");
}

static void
view_size_allocated_cb (GtkWidget *widget, GtkAllocation *allocation, ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  priv->viewport_size.width = allocation->width;
  priv->viewport_size.height = allocation->height;

  resize_viewport(view);
  map_load_visible_tiles (priv->map, priv->viewport_size, priv->offline);
  
  // Setup mouse cursor to a hand
  gdk_window_set_cursor( priv->clutter_embed->window, priv->cursor_hand_open);
}
static gboolean 
mouse_button_cb (GtkWidget *widget, GdkEventButton *event, ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  
  if (event->type == GDK_BUTTON_PRESS)
    gdk_window_set_cursor( priv->clutter_embed->window, priv->cursor_hand_closed);
  else
    gdk_window_set_cursor( priv->clutter_embed->window, priv->cursor_hand_open);

  return FALSE;
}

/**
 * champlain_view_new:
 * @mode: a #ChamplainViewMode, the scrolling mode
 * Returns a new #ChamplainWidget ready to be used as a #GtkWidget.
 *
 * Since: 0.1
 */
GtkWidget *
champlain_view_new (ChamplainViewMode mode)
{
  ClutterColor stage_color = { 0x34, 0x39, 0x39, 0xff };
  ChamplainView *view;
  ClutterActor *stage;

  view = CHAMPLAIN_VIEW (g_object_new (CHAMPLAIN_TYPE_VIEW, NULL));
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  priv->clutter_embed = gtk_clutter_embed_new ();
  g_signal_connect (priv->clutter_embed,
                    "size-allocate",
                    G_CALLBACK (view_size_allocated_cb),
                    view);
  g_signal_connect (priv->clutter_embed,
                    "button-press-event",
                    G_CALLBACK (mouse_button_cb),
                    view);
  g_signal_connect (priv->clutter_embed,
                    "button-release-event",
                    G_CALLBACK (mouse_button_cb),
                    view);
  // Setup cursors
  priv->cursor_hand_open = gdk_cursor_new(GDK_HAND1);
  priv->cursor_hand_closed = gdk_cursor_new(GDK_FLEUR);
  
  // Setup stage
  stage = gtk_clutter_embed_get_stage (GTK_CLUTTER_EMBED (priv->clutter_embed));

  clutter_stage_set_color (CLUTTER_STAGE (stage), &stage_color);
  gtk_container_add (GTK_CONTAINER (view), priv->clutter_embed);
  g_signal_connect (stage,
                    "scroll-event",
                    G_CALLBACK (scroll_event),
                    view);

  // Setup viewport
  priv->viewport = tidy_viewport_new ();
  g_object_set (G_OBJECT (priv->viewport), "sync-adjustments", FALSE, NULL);

  g_signal_connect (priv->viewport,
                    "notify::x-origin",
                    G_CALLBACK (viewport_x_changed_cb),
                    view);

  // Setup finger scroll
  TidyFingerScrollMode tidy_mode;
  switch(mode)
    {
      case CHAMPLAIN_VIEW_MODE_PUSH:
        tidy_mode = TIDY_FINGER_SCROLL_MODE_PUSH;
        break;
      case CHAMPLAIN_VIEW_MODE_KINETIC:
        tidy_mode = TIDY_FINGER_SCROLL_MODE_KINETIC;
        break;
    }

  priv->finger_scroll = tidy_finger_scroll_new(mode);

  clutter_container_add_actor (CLUTTER_CONTAINER (priv->finger_scroll), priv->viewport);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), priv->finger_scroll);

  // Map Layer
  priv->map_layer = clutter_group_new();
  clutter_actor_show(priv->map_layer);
  clutter_container_add_actor (CLUTTER_CONTAINER (priv->viewport), priv->map_layer);

  // Setup user_layers
  priv->user_layers = clutter_group_new();
  clutter_actor_show(priv->user_layers);
  clutter_container_add_actor (CLUTTER_CONTAINER (priv->viewport), priv->user_layers);
  clutter_actor_raise(priv->user_layers, priv->map_layer);

  return GTK_WIDGET (view);
}

/**
 * champlain_view_center_on:
 * @view: a #ChamplainView
 * @longitude: the longitude to center the map at
 * @latitude: the longitude to center the map at
 *
 * Centers the map on these coordinates.
 *
 * Since: 0.1
 */
// FIXME: Animate this.  Can be done in Tidy-Adjustment (like for elastic effect)
void
champlain_view_center_on (ChamplainView *view, gdouble longitude, gdouble latitude)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);

  if(!priv->map)
    {
      create_initial_map(view);
    }

  gdouble x, y;
  x = priv->map->longitude_to_x(priv->map, longitude, priv->map->current_level->level);
  y = priv->map->latitude_to_y(priv->map, latitude, priv->map->current_level->level);
  GdkPoint* anchor = &priv->map->current_level->anchor;

  if (priv->map->current_level->level >= 8) {
    anchor->x = x - G_MAXINT16 / 2;
    anchor->y = y - G_MAXINT16 / 2;

    if ( anchor->x < 0 )
      anchor->x = 0;
    if ( anchor->y < 0 )
      anchor->y = 0;

    gdouble max = zoom_level_get_width(priv->map->current_level) - (G_MAXINT16 / 2);
    if (anchor->x > max)
      anchor->x = max;
    if (anchor->y > max)
      anchor->y = max;

    x -= anchor->x;
    y -= anchor->y;
  } else {
    anchor->x = 0;
    anchor->y = 0;
  }

  int i;
  for (i = 0; i < priv->map->current_level->tiles->len; i++)
    {
      Tile *tile = g_ptr_array_index(priv->map->current_level->tiles, i);
      if (!tile->loading)
        tile_set_position(priv->map, tile);
    }

  tidy_viewport_set_origin(TIDY_VIEWPORT(priv->viewport),
    x - priv->viewport_size.width / 2.0,
    y - priv->viewport_size.height / 2.0,
    0);

  g_object_notify(G_OBJECT(view), "longitude");
  g_object_notify(G_OBJECT(view), "latitude");

  map_load_visible_tiles (priv->map, priv->viewport_size, priv->offline);
}

/**
 * champlain_view_zoom_in:
 * @view: a #ChamplainView
 *
 * Zoom in the map by one level.
 *
 * Since: 0.1
 */
void
champlain_view_zoom_in (ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  ClutterActor *group = priv->map->current_level->group;
  gdouble lon = viewport_get_current_longitude(priv);
  gdouble lat = viewport_get_current_latitude(priv);

  if(map_zoom_in(priv->map))
    {
      resize_viewport(view);
      clutter_container_remove_actor (CLUTTER_CONTAINER (priv->map_layer), group);
      clutter_container_add_actor (CLUTTER_CONTAINER (priv->map_layer), priv->map->current_level->group);
      champlain_view_center_on(view, lon, lat);
      marker_reposition(view);

      g_object_notify(G_OBJECT(view), "zoom-level");
    }
}

/**
 * champlain_view_zoom_out:
 * @view: a #ChamplainView
 *
 * Zoom out the map by one level.
 *
 * Since: 0.1
 */
void
champlain_view_zoom_out (ChamplainView *view)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  ClutterActor *group = priv->map->current_level->group;
  gdouble lon = viewport_get_current_longitude(priv);
  gdouble lat = viewport_get_current_latitude(priv);

  if(map_zoom_out(priv->map))
    {
      resize_viewport(view);
      clutter_container_remove_actor (CLUTTER_CONTAINER (priv->map_layer), group);
      clutter_container_add_actor (CLUTTER_CONTAINER (priv->map_layer), priv->map->current_level->group);
      champlain_view_center_on(view, lon, lat);
      marker_reposition(view);

      g_object_notify(G_OBJECT(view), "zoom-level");
    }
}

/**
 * champlain_view_add_layer:
 * @view: a #ChamplainView
 * @layer: a #ClutterActor
 *
 * Adds a new layer to the view
 *
 * Since: 0.2
 */
void
champlain_view_add_layer (ChamplainView *view, ClutterActor *layer)
{
  ChamplainViewPrivate *priv = CHAMPLAIN_VIEW_GET_PRIVATE (view);
  clutter_container_add(CLUTTER_CONTAINER(priv->user_layers), layer, NULL);
  clutter_actor_raise_top(layer);

  if(priv->map)
    marker_reposition(view);

  g_signal_connect (layer,
                    "add",
                    G_CALLBACK (layer_add_marker_cb),
                    view);

  clutter_container_foreach(CLUTTER_CONTAINER(layer), CLUTTER_CALLBACK(connect_marker_notify_cb), view);
}
