/* champlain-adjustment.c: Adjustment object
 *
 * Copyright (C) 2008 OpenedHand
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Written by: Chris Lord <chris@openedhand.com>, inspired by GtkAdjustment
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib-object.h>
#include <clutter/clutter.h>

#include "champlain-adjustment.h"
#include "champlain-marshal.h"
#include "champlain-private.h"

G_DEFINE_TYPE (ChamplainAdjustment, champlain_adjustment, G_TYPE_OBJECT)

#define ADJUSTMENT_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CHAMPLAIN_TYPE_ADJUSTMENT, ChamplainAdjustmentPrivate))

struct _ChamplainAdjustmentPrivate
{
  gdouble lower;
  gdouble upper;
  gdouble value;
  gdouble step_increment;
  gdouble page_increment;
  gdouble page_size;

  /* For interpolation */
  ClutterTimeline *interpolation;
  gdouble     dx;
  gdouble     old_position;
  gdouble     new_position;

  /* For elasticity */
  gboolean      elastic;
  ClutterAlpha *bounce_alpha;
};

enum
{
  PROP_0,

  PROP_LOWER,
  PROP_UPPER,
  PROP_VALUE,
  PROP_STEP_INC,
  PROP_PAGE_INC,
  PROP_PAGE_SIZE,

  PROP_ELASTIC,
};

enum
{
  CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

static void champlain_adjustment_set_lower          (ChamplainAdjustment *adjustment,
                                                gdouble         lower);
static void champlain_adjustment_set_upper          (ChamplainAdjustment *adjustment,
                                                gdouble         upper);
static void champlain_adjustment_set_step_increment (ChamplainAdjustment *adjustment,
                                                gdouble         step);
static void champlain_adjustment_set_page_increment (ChamplainAdjustment *adjustment,
                                                gdouble         page);
static void champlain_adjustment_set_page_size      (ChamplainAdjustment *adjustment,
                                                gdouble         size);

static void
champlain_adjustment_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ChamplainAdjustmentPrivate *priv = CHAMPLAIN_ADJUSTMENT (object)->priv;

  switch (prop_id)
    {
    case PROP_LOWER:
      g_value_set_double (value, priv->lower);
      break;

    case PROP_UPPER:
      g_value_set_double (value, priv->upper);
      break;

    case PROP_VALUE:
      g_value_set_double (value, priv->value);
      break;

    case PROP_STEP_INC:
      g_value_set_double (value, priv->step_increment);
      break;

    case PROP_PAGE_INC:
      g_value_set_double (value, priv->page_increment);
      break;

    case PROP_PAGE_SIZE:
      g_value_set_double (value, priv->page_size);
      break;

    case PROP_ELASTIC:
      g_value_set_boolean (value, priv->elastic);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
champlain_adjustment_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  ChamplainAdjustment *adj = CHAMPLAIN_ADJUSTMENT (object);

  switch (prop_id)
    {
    case PROP_LOWER:
      champlain_adjustment_set_lower (adj, g_value_get_double (value));
      break;

    case PROP_UPPER:
      champlain_adjustment_set_upper (adj, g_value_get_double (value));
      break;

    case PROP_VALUE:
      champlain_adjustment_set_value (adj, g_value_get_double (value));
      break;

    case PROP_STEP_INC:
      champlain_adjustment_set_step_increment (adj, g_value_get_double (value));
      break;

    case PROP_PAGE_INC:
      champlain_adjustment_set_page_increment (adj, g_value_get_double (value));
      break;

    case PROP_PAGE_SIZE:
      champlain_adjustment_set_page_size (adj, g_value_get_double (value));
      break;

    case PROP_ELASTIC:
      champlain_adjustment_set_elastic (adj, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
stop_interpolation (ChamplainAdjustment *adjustment)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;
  if (priv->interpolation)
    {
      clutter_timeline_stop (priv->interpolation);
      g_object_unref (priv->interpolation);
      priv->interpolation = NULL;

      if (priv->bounce_alpha)
        {
          g_object_unref (priv->bounce_alpha);
          priv->bounce_alpha = NULL;
        }
    }
}

void
champlain_adjustment_interpolate_stop (ChamplainAdjustment *adjustment)
{
  stop_interpolation (adjustment);
}

static void
champlain_adjustment_dispose (GObject *object)
{
  stop_interpolation (CHAMPLAIN_ADJUSTMENT (object));
  
  G_OBJECT_CLASS (champlain_adjustment_parent_class)->dispose (object);
}

static void
champlain_adjustment_class_init (ChamplainAdjustmentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (ChamplainAdjustmentPrivate));

  object_class->get_property = champlain_adjustment_get_property;
  object_class->set_property = champlain_adjustment_set_property;
  object_class->dispose = champlain_adjustment_dispose;
  
  g_object_class_install_property (object_class,
                                   PROP_LOWER,
                                   g_param_spec_double ("lower",
                                                        "Lower",
                                                        "Lower bound",
                                                        -G_MAXDOUBLE,
                                                        G_MAXDOUBLE,
                                                        0.0,
                                                        CHAMPLAIN_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_UPPER,
                                   g_param_spec_double ("upper",
                                                        "Upper",
                                                        "Upper bound",
                                                        -G_MAXDOUBLE,
                                                        G_MAXDOUBLE,
                                                        0.0,
                                                        CHAMPLAIN_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_VALUE,
                                   g_param_spec_double ("value",
                                                        "Value",
                                                        "Current value",
                                                        -G_MAXDOUBLE,
                                                        G_MAXDOUBLE,
                                                        0.0,
                                                        CHAMPLAIN_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_STEP_INC,
                                   g_param_spec_double ("step-increment",
                                                        "Step Increment",
                                                        "Step increment",
                                                        -G_MAXDOUBLE,
                                                        G_MAXDOUBLE,
                                                        0.0,
                                                        CHAMPLAIN_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_PAGE_INC,
                                   g_param_spec_double ("page-increment",
                                                        "Page Increment",
                                                        "Page increment",
                                                        -G_MAXDOUBLE,
                                                        G_MAXDOUBLE,
                                                        0.0,
                                                        CHAMPLAIN_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_PAGE_SIZE,
                                   g_param_spec_double ("page-size",
                                                        "Page Size",
                                                        "Page size",
                                                        -G_MAXDOUBLE,
                                                        G_MAXDOUBLE,
                                                        0.0,
                                                        CHAMPLAIN_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ELASTIC,
                                   g_param_spec_boolean ("elastic",
                                                         "Elastic",
                                                         "Make interpolation "
                                                         "behave in an "
                                                         "'elastic' way and "
                                                         "stop clamping value.",
                                                         FALSE,
                                                         CHAMPLAIN_PARAM_READWRITE));

  signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ChamplainAdjustmentClass, changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

static void
champlain_adjustment_init (ChamplainAdjustment *self)
{
  self->priv = ADJUSTMENT_PRIVATE (self);
}

ChamplainAdjustment *
champlain_adjustment_new (gdouble value,
                     gdouble lower,
                     gdouble upper,
                     gdouble step_increment,
                     gdouble page_increment,
                     gdouble page_size)
{
  return g_object_new (CHAMPLAIN_TYPE_ADJUSTMENT,
                       "value", value,
                       "lower", lower,
                       "upper", upper,
                       "step-increment", step_increment,
                       "page-increment", page_increment,
                       "page-size", page_size,
                       NULL);
}

gdouble
champlain_adjustment_get_value (ChamplainAdjustment *adjustment)
{
  g_return_val_if_fail (CHAMPLAIN_IS_ADJUSTMENT (adjustment), 0.0);

  return adjustment->priv->value;
}

void
champlain_adjustment_set_value (ChamplainAdjustment *adjustment,
                           double value)
{
  ChamplainAdjustmentPrivate *priv;

  g_return_if_fail (CHAMPLAIN_IS_ADJUSTMENT (adjustment));

  priv = adjustment->priv;

  stop_interpolation (adjustment);

  if (!priv->elastic)
    value = CLAMP (value, priv->lower, MAX (priv->lower,
                                            priv->upper - priv->page_size));

  if (priv->value != value)
    {
      priv->value = value;
      g_object_notify (G_OBJECT (adjustment), "value");
    }
}

static void
champlain_adjustment_clamp_page (ChamplainAdjustment *adjustment,
                            double lower,
                            double upper)
{
  gboolean changed;
  ChamplainAdjustmentPrivate *priv;

  g_return_if_fail (CHAMPLAIN_IS_ADJUSTMENT (adjustment));

  priv = adjustment->priv;

  stop_interpolation (adjustment);

  lower = CLAMP (lower, priv->lower, priv->upper - priv->page_size);
  upper = CLAMP (upper, priv->lower + priv->page_size, priv->upper);

  changed = FALSE;

  if (priv->value + priv->page_size > upper)
    {
      priv->value = upper - priv->page_size;
      changed = TRUE;
    }

  if (priv->value < lower)
    {
      priv->value = lower;
      changed = TRUE;
    }

  if (changed)
    g_object_notify (G_OBJECT (adjustment), "value");
}

static void
champlain_adjustment_set_lower (ChamplainAdjustment *adjustment,
                           gdouble         lower)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;

  if (priv->lower != lower)
    {
      priv->lower = lower;

      g_signal_emit (adjustment, signals[CHANGED], 0);

      g_object_notify (G_OBJECT (adjustment), "lower");

      champlain_adjustment_clamp_page (adjustment, priv->lower, priv->upper);
    }
}

static void
champlain_adjustment_set_upper (ChamplainAdjustment *adjustment,
                           gdouble         upper)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;

  if (priv->upper != upper)
    {
      priv->upper = upper;

      g_signal_emit (adjustment, signals[CHANGED], 0);

      g_object_notify (G_OBJECT (adjustment), "upper");

      champlain_adjustment_clamp_page (adjustment, priv->lower, priv->upper);
    }
}

static void
champlain_adjustment_set_step_increment (ChamplainAdjustment *adjustment,
                                    gdouble         step)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;

  if (priv->step_increment != step)
    {
      priv->step_increment = step;

      g_signal_emit (adjustment, signals[CHANGED], 0);

      g_object_notify (G_OBJECT (adjustment), "step-increment");
    }
}

static void
champlain_adjustment_set_page_increment (ChamplainAdjustment *adjustment,
                                    gdouble        page)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;

  if (priv->page_increment != page)
    {
      priv->page_increment = page;

      g_signal_emit (adjustment, signals[CHANGED], 0);

      g_object_notify (G_OBJECT (adjustment), "page-increment");
    }
}

static void
champlain_adjustment_set_page_size (ChamplainAdjustment *adjustment,
                               gdouble         size)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;

  if (priv->page_size != size)
    {
      priv->page_size = size;

      g_signal_emit (adjustment, signals[CHANGED], 0);

      g_object_notify (G_OBJECT (adjustment), "page_size");

      champlain_adjustment_clamp_page (adjustment, priv->lower, priv->upper);
    }
}

void
champlain_adjustment_get_values (ChamplainAdjustment *adjustment,
                            gdouble        *value,
                            gdouble        *lower,
                            gdouble        *upper,
                            gdouble        *step_increment,
                            gdouble        *page_increment,
                            gdouble        *page_size)
{
  ChamplainAdjustmentPrivate *priv;

  g_return_if_fail (CHAMPLAIN_IS_ADJUSTMENT (adjustment));

  priv = adjustment->priv;

  if (lower)
    *lower = priv->lower;

  if (upper)
    *upper = priv->upper;

  if (value)
    *value = champlain_adjustment_get_value (adjustment);

  if (step_increment)
    *step_increment = priv->step_increment;

  if (page_increment)
    *page_increment = priv->page_increment;

  if (page_size)
    *page_size = priv->page_size;
}

static void
interpolation_new_frame_cb (ClutterTimeline *timeline,
                            gint             frame_num,
                            ChamplainAdjustment  *adjustment)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;

  priv->interpolation = NULL;
  if (priv->elastic && priv->bounce_alpha)
    {
      gdouble progress = clutter_alpha_get_alpha (priv->bounce_alpha) / 1;
      gdouble dx = priv->old_position +
        (priv->new_position - priv->old_position) *
        progress;
      champlain_adjustment_set_value (adjustment, dx);
    }
  else
    champlain_adjustment_set_value (adjustment,
                                priv->old_position +
                                frame_num * priv->dx);
  priv->interpolation = timeline;
}

static void
interpolation_completed_cb (ClutterTimeline *timeline,
                            ChamplainAdjustment  *adjustment)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;
 
  stop_interpolation (adjustment);
  champlain_adjustment_set_value (adjustment,
                              priv->new_position);
}

/* Note, there's super-optimal code that does a similar thing in
 * clutter-alpha.c
 *
 * Tried this instead of CLUTTER_ALPHA_SINE_INC, but I think SINE_INC looks
 * better. Leaving code here in case this is revisited.
 */
/*
static guint32
bounce_alpha_func (ClutterAlpha *alpha,
                   gpointer      user_data)
{
  gdouble progress, angle;
  ClutterTimeline *timeline = clutter_alpha_get_timeline (alpha);
  
  progress = clutter_timeline_get_progressx (timeline);
  angle = clutter_qmulx (CFX_PI_2 + CFX_PI_4/2, progress);
  
  return clutter_sinx (angle) +
    (CFX_ONE - clutter_sinx (CFX_PI_2 + CFX_PI_4/2));
}
*/

void
champlain_adjustment_interpolate (ChamplainAdjustment *adjustment,
                              gdouble         value,
                              guint           n_frames,
                              guint           fps)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;

  stop_interpolation (adjustment);

  if (n_frames <= 1)
    {
      champlain_adjustment_set_value (adjustment, value);
      return;
    }

  priv->old_position = priv->value;
  priv->new_position = value;

  priv->dx = (priv->new_position - priv->old_position) * n_frames;
  priv->interpolation = clutter_timeline_new (((float)n_frames / fps) * 1000);

  if (priv->elastic)
    priv->bounce_alpha = clutter_alpha_new_full (priv->interpolation,
                                                 CLUTTER_EASE_OUT_SINE);

  g_signal_connect (priv->interpolation,
                    "new-frame",
                    G_CALLBACK (interpolation_new_frame_cb),
                    adjustment);
  g_signal_connect (priv->interpolation,
                    "completed",
                    G_CALLBACK (interpolation_completed_cb),
                    adjustment);

  clutter_timeline_start (priv->interpolation);
}

gboolean
champlain_adjustment_get_elastic (ChamplainAdjustment *adjustment)
{
  return adjustment->priv->elastic;
}

void
champlain_adjustment_set_elastic (ChamplainAdjustment *adjustment,
                             gboolean        elastic)
{
  adjustment->priv->elastic = elastic;
}

gboolean
champlain_adjustment_clamp (ChamplainAdjustment *adjustment,
                       gboolean        interpolate,
                       guint           n_frames,
                       guint           fps)
{
  ChamplainAdjustmentPrivate *priv = adjustment->priv;
  gdouble dest = priv->value;

  if (priv->value < priv->lower)
    dest = priv->lower;
  if (priv->value > priv->upper - priv->page_size)
    dest = priv->upper - priv->page_size;

  if (dest != priv->value)
    {
      if (interpolate)
        champlain_adjustment_interpolate (adjustment,
                                      dest,
                                      n_frames,
                                      fps);
      else
        champlain_adjustment_set_value (adjustment, dest);

      return TRUE;
    }

  return FALSE;
}
