/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
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
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/video/gstvideometa.h>

#include <gst/gl/gl.h>

#define GST_CAT_DEFAULT gst_gl_base_filter_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define GST_GL_BASE_FILTER_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE((o), GST_TYPE_GL_BASE_FILTER, GstGLBaseFilterPrivate))

struct _GstGLBaseFilterPrivate
{
  GstGLContext *other_context;

  gboolean gl_result;
};

/* Properties */
enum
{
  PROP_0,
  PROP_CONTEXT
};

#define gst_gl_base_filter_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGLBaseFilter, gst_gl_base_filter,
    GST_TYPE_BASE_TRANSFORM, GST_DEBUG_CATEGORY_INIT (gst_gl_base_filter_debug,
        "glbasefilter", 0, "glbasefilter element");
    );

static void gst_gl_base_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_base_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void gst_gl_base_filter_set_context (GstElement * element,
    GstContext * context);
static gboolean gst_gl_base_filter_query (GstBaseTransform * trans,
    GstPadDirection direction, GstQuery * query);
static void gst_gl_base_filter_reset (GstGLBaseFilter * filter);
static gboolean gst_gl_base_filter_start (GstBaseTransform * bt);
static gboolean gst_gl_base_filter_stop (GstBaseTransform * bt);
static gboolean gst_gl_base_filter_decide_allocation (GstBaseTransform * trans,
    GstQuery * query);
static gboolean gst_gl_base_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);

/* GstGLContextThreadFunc */
static void gst_gl_base_filter_gl_start (GstGLContext * context, gpointer data);
static void gst_gl_base_filter_gl_stop (GstGLContext * context, gpointer data);

static void
gst_gl_base_filter_class_init (GstGLBaseFilterClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  g_type_class_add_private (klass, sizeof (GstGLBaseFilterPrivate));

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_gl_base_filter_set_property;
  gobject_class->get_property = gst_gl_base_filter_get_property;

  GST_BASE_TRANSFORM_CLASS (klass)->query = gst_gl_base_filter_query;
  GST_BASE_TRANSFORM_CLASS (klass)->start = gst_gl_base_filter_start;
  GST_BASE_TRANSFORM_CLASS (klass)->stop = gst_gl_base_filter_stop;
  GST_BASE_TRANSFORM_CLASS (klass)->decide_allocation =
      gst_gl_base_filter_decide_allocation;
  GST_BASE_TRANSFORM_CLASS (klass)->propose_allocation =
      gst_gl_base_filter_propose_allocation;

  element_class->set_context = gst_gl_base_filter_set_context;

  g_object_class_install_property (gobject_class, PROP_CONTEXT,
      g_param_spec_object ("context",
          "OpenGL context",
          "Get OpenGL context",
          GST_GL_TYPE_CONTEXT, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  klass->supported_gl_api = GST_GL_API_ANY;
}

static void
gst_gl_base_filter_init (GstGLBaseFilter * filter)
{
  filter->priv = GST_GL_BASE_FILTER_GET_PRIVATE (filter);
}

static void
gst_gl_base_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_base_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (object);

  switch (prop_id) {
    case PROP_CONTEXT:
      g_value_set_object (value, filter->context);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_base_filter_set_context (GstElement * element, GstContext * context)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (element);
  GstGLBaseFilterClass *filter_class = GST_GL_BASE_FILTER_GET_CLASS (filter);

  gst_gl_handle_set_context (element, context, &filter->display,
      &filter->priv->other_context);
  if (filter->display)
    gst_gl_display_filter_gl_api (filter->display,
        filter_class->supported_gl_api);
}

static gboolean
_find_local_gl_context (GstGLBaseFilter * filter)
{
  GstQuery *query;
  GstContext *context;
  const GstStructure *s;

  if (filter->context)
    return TRUE;

  query = gst_query_new_context ("gst.gl.local_context");
  if (!filter->context
      && gst_gl_run_query (GST_ELEMENT (filter), query, GST_PAD_SRC)) {
    gst_query_parse_context (query, &context);
    if (context) {
      s = gst_context_get_structure (context);
      gst_structure_get (s, "context", GST_GL_TYPE_CONTEXT, &filter->context,
          NULL);
    }
  }
  if (!filter->context
      && gst_gl_run_query (GST_ELEMENT (filter), query, GST_PAD_SINK)) {
    gst_query_parse_context (query, &context);
    if (context) {
      s = gst_context_get_structure (context);
      gst_structure_get (s, "context", GST_GL_TYPE_CONTEXT, &filter->context,
          NULL);
    }
  }

  GST_DEBUG_OBJECT (filter, "found local context %p", filter->context);

  gst_query_unref (query);

  if (filter->context)
    return TRUE;

  return FALSE;
}

static gboolean
_ensure_gl_setup (GstGLBaseFilter * filter)
{
  GstGLBaseFilterClass *filter_class = GST_GL_BASE_FILTER_GET_CLASS (filter);

  if (!gst_gl_ensure_element_data (filter, &filter->display,
          &filter->priv->other_context)) {
    return FALSE;
  }

  gst_gl_display_filter_gl_api (filter->display,
      filter_class->supported_gl_api);

  _find_local_gl_context (filter);

  return TRUE;
}

static gboolean
gst_gl_base_filter_query (GstBaseTransform * trans, GstPadDirection direction,
    GstQuery * query)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (trans);
  GstGLBaseFilterClass *filter_class = GST_GL_BASE_FILTER_GET_CLASS (filter);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ALLOCATION:
    {
      if (direction == GST_PAD_SINK
          && gst_base_transform_is_passthrough (trans)) {
        if (!_ensure_gl_setup (filter))
          return FALSE;

        return gst_pad_peer_query (GST_BASE_TRANSFORM_SRC_PAD (trans), query);
      }
      break;
    }
    case GST_QUERY_CONTEXT:
    {
      const gchar *context_type;
      GstContext *context, *old_context;
      gboolean ret;

      ret = gst_gl_handle_context_query ((GstElement *) filter, query,
          &filter->display, &filter->priv->other_context);
      if (filter->display)
        gst_gl_display_filter_gl_api (filter->display,
            filter_class->supported_gl_api);

      gst_query_parse_context_type (query, &context_type);

      if (g_strcmp0 (context_type, "gst.gl.local_context") == 0) {
        GstStructure *s;

        gst_query_parse_context (query, &old_context);

        if (old_context)
          context = gst_context_copy (old_context);
        else
          context = gst_context_new ("gst.gl.local_context", FALSE);

        s = gst_context_writable_structure (context);
        gst_structure_set (s, "context", GST_GL_TYPE_CONTEXT, filter->context,
            NULL);
        gst_query_set_context (query, context);
        gst_context_unref (context);

        ret = filter->context != NULL;
      }
      GST_LOG_OBJECT (filter, "context query of type %s %i", context_type, ret);

      if (ret)
        return ret;
      break;
    }
    default:
      break;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->query (trans, direction,
      query);
}

static void
gst_gl_base_filter_reset (GstGLBaseFilter * filter)
{
  GstGLBaseFilterClass *filter_class = GST_GL_BASE_FILTER_GET_CLASS (filter);

  if (filter->context) {
    if (filter_class->gl_stop != NULL) {
      gst_gl_context_thread_add (filter->context, gst_gl_base_filter_gl_stop,
          filter);
    }

    gst_object_unref (filter->context);
    filter->context = NULL;
  }

  if (filter->display) {
    gst_object_unref (filter->display);
    filter->display = NULL;
  }

  if (filter->priv->other_context) {
    gst_object_unref (filter->priv->other_context);
    filter->priv->other_context = NULL;
  }
}

static gboolean
gst_gl_base_filter_start (GstBaseTransform * bt)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (bt);
  GstGLBaseFilterClass *filter_class = GST_GL_BASE_FILTER_GET_CLASS (filter);

  if (!gst_gl_ensure_element_data (filter, &filter->display,
          &filter->priv->other_context))
    return FALSE;

  gst_gl_display_filter_gl_api (filter->display,
      filter_class->supported_gl_api);

  return TRUE;
}

static gboolean
gst_gl_base_filter_stop (GstBaseTransform * bt)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (bt);

  gst_gl_base_filter_reset (filter);

  return TRUE;
}

static void
gst_gl_base_filter_gl_start (GstGLContext * context, gpointer data)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (data);
  GstGLBaseFilterClass *filter_class = GST_GL_BASE_FILTER_GET_CLASS (filter);

  if (filter_class->gl_start) {
    filter->priv->gl_result = filter_class->gl_start (filter);
  } else {
    filter->priv->gl_result = TRUE;
  }
}

static void
gst_gl_base_filter_gl_stop (GstGLContext * context, gpointer data)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (data);
  GstGLBaseFilterClass *filter_class = GST_GL_BASE_FILTER_GET_CLASS (filter);

  if (filter_class->gl_stop)
    filter_class->gl_stop (filter);
}

static gboolean
gst_gl_base_filter_decide_allocation (GstBaseTransform * trans,
    GstQuery * query)
{
  GstGLBaseFilter *filter = GST_GL_BASE_FILTER (trans);
  GError *error = NULL;
  GstGLContext *other_context = NULL;
  guint idx;

  if (!_ensure_gl_setup (filter))
    return FALSE;

  if (!filter->context && gst_query_find_allocation_meta (query,
          GST_VIDEO_GL_TEXTURE_UPLOAD_META_API_TYPE, &idx)) {
    GstGLContext *context;
    const GstStructure *upload_meta_params;
    gpointer handle;
    gchar *type;
    gchar *apis;

    gst_query_parse_nth_allocation_meta (query, idx, &upload_meta_params);
    if (upload_meta_params) {
      if (gst_structure_get (upload_meta_params, "gst.gl.GstGLContext",
              GST_GL_TYPE_CONTEXT, &context, NULL) && context) {
        GstGLContext *old = filter->context;

        filter->context = context;
        if (old)
          gst_object_unref (old);
      } else if (gst_structure_get (upload_meta_params, "gst.gl.context.handle",
              G_TYPE_POINTER, &handle, "gst.gl.context.type", G_TYPE_STRING,
              &type, "gst.gl.context.apis", G_TYPE_STRING, &apis, NULL)
          && handle) {
        GstGLPlatform platform = GST_GL_PLATFORM_NONE;
        GstGLAPI gl_apis;

        GST_DEBUG ("got GL context handle 0x%p with type %s and apis %s",
            handle, type, apis);

        platform = gst_gl_platform_from_string (type);
        gl_apis = gst_gl_api_from_string (apis);

        if (gl_apis && platform)
          other_context =
              gst_gl_context_new_wrapped (filter->display, (guintptr) handle,
              platform, gl_apis);
      }
    }
  }

  if (filter->priv->other_context) {
    if (!other_context) {
      other_context = filter->priv->other_context;
    } else {
      GST_ELEMENT_WARNING (filter, LIBRARY, SETTINGS,
          ("%s", "Cannot share with more than one GL context"),
          ("%s", "Cannot share with more than one GL context"));
    }
  }

  if (!filter->context) {
    filter->context = gst_gl_context_new (filter->display);
    if (!gst_gl_context_create (filter->context, other_context, &error))
      goto context_error;
  }

  gst_gl_context_thread_add (filter->context, gst_gl_base_filter_gl_start,
      filter);
  if (!filter->priv->gl_result)
    goto error;

  return TRUE;

context_error:
  {
    GST_ELEMENT_ERROR (trans, RESOURCE, NOT_FOUND, ("%s", error->message),
        (NULL));
    return FALSE;
  }
error:
  {
    GST_ELEMENT_ERROR (trans, LIBRARY, INIT,
        ("Subclass failed to initialize."), (NULL));
    return FALSE;
  }
}

static gboolean
gst_gl_base_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  return FALSE;
}