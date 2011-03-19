/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2006 OpenedHand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

/*
 * SECTION:champlain-group
 * @short_description: A fixed layout container
 *
 * This is a shameless coppy of ClutterGroup. The only difference is that
 * it doesn't sort actors by depth, which slows down things in libchamplain
 * considerably.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>

#include "champlain-group.h"

#include <clutter/clutter.h>

#define CHAMPLAIN_GROUP_GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), CHAMPLAIN_TYPE_GROUP, ChamplainGroupPrivate))

struct _ChamplainGroupPrivate
{
  GList *children;

  ClutterLayoutManager *layout;
};

enum
{
  ADD,
  REMOVE,

  LAST_SIGNAL
};

static void clutter_container_iface_init (ClutterContainerIface *iface);

G_DEFINE_TYPE_WITH_CODE (ChamplainGroup, champlain_group, CLUTTER_TYPE_ACTOR,
    G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_CONTAINER,
        clutter_container_iface_init));

/*
   static gint
   sort_by_depth (gconstpointer a,
               gconstpointer b)
   {
   gfloat depth_a = clutter_actor_get_depth (CLUTTER_ACTOR(a));
   gfloat depth_b = clutter_actor_get_depth (CLUTTER_ACTOR(b));

   if (depth_a < depth_b)
    return -1;

   if (depth_a > depth_b)
    return 1;

   return 0;
   }
 */

static void
champlain_group_real_add (ClutterContainer *container,
    ClutterActor *actor)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (container)->priv;

  g_object_ref (actor);

  priv->children = g_list_append (priv->children, actor);
  clutter_actor_set_parent (actor, CLUTTER_ACTOR (container));

  /* queue a relayout, to get the correct positioning inside
   * the ::actor-added signal handlers
   */
  clutter_actor_queue_relayout (CLUTTER_ACTOR (container));

  g_signal_emit_by_name (container, "actor-added", actor);

  clutter_container_sort_depth_order (container);

  g_object_unref (actor);
}


static void
champlain_group_real_remove (ClutterContainer *container,
    ClutterActor *actor)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (container)->priv;

  g_object_ref (actor);

  priv->children = g_list_remove (priv->children, actor);
  clutter_actor_unparent (actor);

  /* queue a relayout, to get the correct positioning inside
   * the ::actor-removed signal handlers
   */
  clutter_actor_queue_relayout (CLUTTER_ACTOR (container));

  /* at this point, the actor passed to the "actor-removed" signal
   * handlers is not parented anymore to the container but since we
   * are holding a reference on it, it's still valid
   */
  g_signal_emit_by_name (container, "actor-removed", actor);

  clutter_actor_queue_redraw (CLUTTER_ACTOR (container));

  g_object_unref (actor);
}


static void
champlain_group_real_foreach (ClutterContainer *container,
    ClutterCallback callback,
    gpointer user_data)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (container)->priv;

  /* Using g_list_foreach instead of iterating the list manually
     because it has better protection against the current node being
     removed. This will happen for example if someone calls
     clutter_container_foreach(container, clutter_actor_destroy) */
  g_list_foreach (priv->children, (GFunc) callback, user_data);
}


static void
champlain_group_real_raise (ClutterContainer *container,
    ClutterActor *actor,
    ClutterActor *sibling)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (container)->priv;

  priv->children = g_list_remove (priv->children, actor);

  /* Raise at the top */
  if (!sibling)
    {
      GList *last_item;

      last_item = g_list_last (priv->children);

      if (last_item)
        sibling = last_item->data;

      priv->children = g_list_append (priv->children, actor);
    }
  else
    {
      gint index_ = g_list_index (priv->children, sibling) + 1;

      priv->children = g_list_insert (priv->children, actor, index_);
    }

  /* set Z ordering a value below, this will then call sort
   * as values are equal ordering shouldn't change but Z
   * values will be correct.
   *
   * FIXME: optimise
   */
/*  if (sibling &&
      clutter_actor_get_depth (sibling) != clutter_actor_get_depth (actor))
    {
      clutter_actor_set_depth (actor, clutter_actor_get_depth (sibling));
    }*/

  clutter_actor_queue_redraw (CLUTTER_ACTOR (container));
}


static void
champlain_group_real_lower (ClutterContainer *container,
    ClutterActor *actor,
    ClutterActor *sibling)
{
  ChamplainGroup *self = CHAMPLAIN_GROUP (container);
  ChamplainGroupPrivate *priv = self->priv;

  priv->children = g_list_remove (priv->children, actor);

  /* Push to bottom */
  if (!sibling)
    {
      GList *last_item;

      last_item = g_list_first (priv->children);

      if (last_item)
        sibling = last_item->data;

      priv->children = g_list_prepend (priv->children, actor);
    }
  else
    {
      gint index_ = g_list_index (priv->children, sibling);

      priv->children = g_list_insert (priv->children, actor, index_);
    }

  /* See comment in group_raise for this */
/*  if (sibling &&
      clutter_actor_get_depth (sibling) != clutter_actor_get_depth (actor))
    {
      clutter_actor_set_depth (actor, clutter_actor_get_depth (sibling));
    }*/

  clutter_actor_queue_redraw (CLUTTER_ACTOR (container));
}


static void
champlain_group_real_sort_depth_order (ClutterContainer *container)
{
/*  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (container)->priv; */

/*  priv->children = g_list_sort (priv->children, sort_by_depth); */

/*  clutter_actor_queue_redraw (CLUTTER_ACTOR (container)); */
}


static void
clutter_container_iface_init (ClutterContainerIface *iface)
{
  iface->add = champlain_group_real_add;
  iface->remove = champlain_group_real_remove;
  iface->foreach = champlain_group_real_foreach;
  iface->raise = champlain_group_real_raise;
  iface->lower = champlain_group_real_lower;
  iface->sort_depth_order = champlain_group_real_sort_depth_order;
}


static void
champlain_group_real_paint (ClutterActor *actor)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (actor)->priv;

  g_list_foreach (priv->children, (GFunc) clutter_actor_paint, NULL);
}


static void
champlain_group_real_pick (ClutterActor *actor,
    const ClutterColor *pick)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (actor)->priv;

  /* Chain up so we get a bounding box pained (if we are reactive) */
  CLUTTER_ACTOR_CLASS (champlain_group_parent_class)->pick (actor, pick);

  g_list_foreach (priv->children, (GFunc) clutter_actor_paint, NULL);
}


static void
champlain_group_real_get_preferred_width (ClutterActor *actor,
    gfloat for_height,
    gfloat *min_width,
    gfloat *natural_width)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (actor)->priv;

  clutter_layout_manager_get_preferred_width (priv->layout,
      CLUTTER_CONTAINER (actor),
      for_height,
      min_width, natural_width);
}


static void
champlain_group_real_get_preferred_height (ClutterActor *actor,
    gfloat for_width,
    gfloat *min_height,
    gfloat *natural_height)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (actor)->priv;

  clutter_layout_manager_get_preferred_height (priv->layout,
      CLUTTER_CONTAINER (actor),
      for_width,
      min_height, natural_height);
}


static void
champlain_group_real_allocate (ClutterActor *actor,
    const ClutterActorBox *allocation,
    ClutterAllocationFlags flags)
{
  ChamplainGroupPrivate *priv = CHAMPLAIN_GROUP (actor)->priv;
  ClutterActorClass *klass;

  klass = CLUTTER_ACTOR_CLASS (champlain_group_parent_class);
  klass->allocate (actor, allocation, flags);

  if (priv->children == NULL)
    return;

  clutter_layout_manager_allocate (priv->layout,
      CLUTTER_CONTAINER (actor),
      allocation, flags);
}


static void
champlain_group_dispose (GObject *object)
{
  ChamplainGroup *self = CHAMPLAIN_GROUP (object);
  ChamplainGroupPrivate *priv = self->priv;

  if (priv->children)
    {
      g_list_foreach (priv->children, (GFunc) clutter_actor_destroy, NULL);
      g_list_free (priv->children);

      priv->children = NULL;
    }

  if (priv->layout)
    {
      clutter_layout_manager_set_container (priv->layout, NULL);
      g_object_unref (priv->layout);
      priv->layout = NULL;
    }

  G_OBJECT_CLASS (champlain_group_parent_class)->dispose (object);
}


static void
champlain_group_real_show_all (ClutterActor *actor)
{
  clutter_container_foreach (CLUTTER_CONTAINER (actor),
      CLUTTER_CALLBACK (clutter_actor_show),
      NULL);
  clutter_actor_show (actor);
}


static void
champlain_group_real_hide_all (ClutterActor *actor)
{
  clutter_actor_hide (actor);
  clutter_container_foreach (CLUTTER_CONTAINER (actor),
      CLUTTER_CALLBACK (clutter_actor_hide),
      NULL);
}


static void
champlain_group_class_init (ChamplainGroupClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  g_type_class_add_private (klass, sizeof (ChamplainGroupPrivate));

  actor_class->get_preferred_width = champlain_group_real_get_preferred_width;
  actor_class->get_preferred_height = champlain_group_real_get_preferred_height;
  actor_class->allocate = champlain_group_real_allocate;
  actor_class->paint = champlain_group_real_paint;
  actor_class->pick = champlain_group_real_pick;
  actor_class->show_all = champlain_group_real_show_all;
  actor_class->hide_all = champlain_group_real_hide_all;

  gobject_class->dispose = champlain_group_dispose;
}


static void
champlain_group_init (ChamplainGroup *self)
{
  self->priv = CHAMPLAIN_GROUP_GET_PRIVATE (self);

  self->priv->layout = clutter_fixed_layout_new ();
  g_object_ref_sink (self->priv->layout);

  clutter_layout_manager_set_container (self->priv->layout,
      CLUTTER_CONTAINER (self));
}


/**
 * champlain_group_new:
 *
 * Create a new  #ChamplainGroup.
 *
 * Return value: the newly created #ChamplainGroup actor
 */
ClutterActor *
champlain_group_new (void)
{
  return g_object_new (CHAMPLAIN_TYPE_GROUP, NULL);
}


/**
 * champlain_group_remove_all:
 * @group: A #ChamplainGroup
 *
 * Removes all children actors from the #ChamplainGroup.
 */
void
champlain_group_remove_all (ChamplainGroup *group)
{
  GList *children;

  g_return_if_fail (CHAMPLAIN_IS_GROUP (group));

  children = group->priv->children;
  while (children)
    {
      ClutterActor *child = children->data;
      children = children->next;

      clutter_container_remove_actor (CLUTTER_CONTAINER (group), child);
    }
}


/**
 * champlain_group_get_n_children:
 * @self: A #ChamplainGroup
 *
 * Gets the number of actors held in the group.
 *
 * Return value: The number of child actors held in the group.
 *
 * Since: 0.2
 */
gint
champlain_group_get_n_children (ChamplainGroup *self)
{
  g_return_val_if_fail (CHAMPLAIN_IS_GROUP (self), 0);

  return g_list_length (self->priv->children);
}


/**
 * champlain_group_get_nth_child:
 * @self: A #ChamplainGroup
 * @index_: the position of the requested actor.
 *
 * Gets a groups child held at @index_ in stack.
 *
 * Return value: (transfer none): A Clutter actor, or %NULL if
 *   @index_ is invalid.
 *
 * Since: 0.2
 */
ClutterActor *
champlain_group_get_nth_child (ChamplainGroup *self,
    gint index_)
{
  g_return_val_if_fail (CHAMPLAIN_IS_GROUP (self), NULL);

  return g_list_nth_data (self->priv->children, index_);
}
