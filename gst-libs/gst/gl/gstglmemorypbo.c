/*
 * GStreamer
 * Copyright (C) 2012 Matthew Waters <ystreet00@gmail.com>
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

#include <string.h>

#include <gst/video/video.h>

#include <gst/gl/gstglmemorypbo.h>

/**
 * SECTION:gstglmemorypbo
 * @short_description: memory subclass for GL textures
 * @see_also: #GstMemory, #GstAllocator, #GstGLBufferPool
 *
 * GstGLMemoryPBO is a #GstGLMemory subclass providing support for the mapping of
 * GL textures.  
 *
 * #GstGLMemoryPBO is created through gst_gl_memory_pbo_alloc() or system memory can
 * be wrapped through gst_gl_memory_pbo_wrapped().
 *
 * Data is uploaded or downloaded from the GPU as is necessary.
 */

/* Implementation notes
 *
 * PBO transfer's are implemented using GstGLBuffer.  We just need to
 * ensure that the texture data is written/read to/from before/after calling
 * map (mem->pbo, READ) which performs the pbo buffer transfer.
 */

#define USING_OPENGL(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL, 1, 0))
#define USING_OPENGL3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL3, 3, 1))
#define USING_GLES(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES, 1, 0))
#define USING_GLES2(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 2, 0))
#define USING_GLES3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 3, 0))

#define GL_MEM_WIDTH(gl_mem) _get_plane_width (&gl_mem->mem.info, gl_mem->mem.plane)
#define GL_MEM_HEIGHT(gl_mem) _get_plane_height (&gl_mem->mem.info, gl_mem->mem.plane)
#define GL_MEM_STRIDE(gl_mem) GST_VIDEO_INFO_PLANE_STRIDE (&gl_mem->mem.info, gl_mem->mem.plane)

#define CONTEXT_SUPPORTS_PBO_UPLOAD(context) \
    (gst_gl_context_check_gl_version (context, \
        GST_GL_API_OPENGL | GST_GL_API_OPENGL3, 2, 1) \
        || gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 3, 0))
#define CONTEXT_SUPPORTS_PBO_DOWNLOAD(context) \
    (gst_gl_context_check_gl_version (context, \
        GST_GL_API_OPENGL | GST_GL_API_OPENGL3 | GST_GL_API_GLES2, 3, 0))

GST_DEBUG_CATEGORY_STATIC (GST_CAT_GL_MEMORY);
#define GST_CAT_DEFAULT GST_CAT_GL_MEMORY

static GstAllocator *_gl_allocator;

/* compatability definitions... */
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_STREAM_COPY
#define GL_STREAM_COPY 0x88E2
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

#define parent_class gst_gl_memory_pbo_allocator_parent_class
G_DEFINE_TYPE (GstGLMemoryPBOAllocator, gst_gl_memory_pbo_allocator,
    GST_TYPE_GL_MEMORY_ALLOCATOR);

typedef struct
{
  /* in */
  GstGLMemoryPBO *src;
  GstVideoGLTextureType out_format;
  guint out_width, out_height;
  guint out_stride;
  gboolean respecify;
  GstGLTextureTarget tex_target;
  /* inout */
  guint tex_id;
  /* out */
  gboolean result;
} GstGLMemoryPBOCopyParams;

static inline guint
_get_plane_width (GstVideoInfo * info, guint plane)
{
  if (GST_VIDEO_INFO_IS_YUV (info))
    /* For now component width and plane width are the same and the
     * plane-component mapping matches
     */
    return GST_VIDEO_INFO_COMP_WIDTH (info, plane);
  else                          /* RGB, GRAY */
    return GST_VIDEO_INFO_WIDTH (info);
}

static inline guint
_get_plane_height (GstVideoInfo * info, guint plane)
{
  if (GST_VIDEO_INFO_IS_YUV (info))
    /* For now component width and plane width are the same and the
     * plane-component mapping matches
     */
    return GST_VIDEO_INFO_COMP_HEIGHT (info, plane);
  else                          /* RGB, GRAY */
    return GST_VIDEO_INFO_HEIGHT (info);
}

static void
_upload_pbo_memory (GstGLMemoryPBO * gl_mem, GstMapInfo * info,
    GstGLBuffer * pbo, GstMapInfo * pbo_info)
{
  GstGLContext *context = gl_mem->mem.mem.context;
  const GstGLFuncs *gl;
  guint gl_format, gl_type, gl_target;
  guint pbo_id;
  gsize plane_start;

  if (!GST_MEMORY_FLAG_IS_SET (gl_mem, GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD))
    return;

  g_return_if_fail (CONTEXT_SUPPORTS_PBO_UPLOAD (context));

  gl = context->gl_vtable;
  pbo_id = *(guint *) pbo_info->data;

  gl_type = GL_UNSIGNED_BYTE;
  if (gl_mem->mem.tex_type == GST_VIDEO_GL_TEXTURE_TYPE_RGB16)
    gl_type = GL_UNSIGNED_SHORT_5_6_5;

  gl_format = gst_gl_format_from_gl_texture_type (gl_mem->mem.tex_type);
  gl_target = gst_gl_texture_target_to_gl (gl_mem->mem.tex_target);

  if (USING_OPENGL (context) || USING_GLES3 (context)
      || USING_OPENGL3 (context)) {
    gl->PixelStorei (GL_UNPACK_ROW_LENGTH, gl_mem->mem.unpack_length);
  } else if (USING_GLES2 (context)) {
    gl->PixelStorei (GL_UNPACK_ALIGNMENT, gl_mem->mem.unpack_length);
  }

  GST_CAT_LOG (GST_CAT_GL_MEMORY, "upload for texture id:%u, with pbo %u %ux%u",
      gl_mem->mem.tex_id, pbo_id, gl_mem->mem.tex_width,
      GL_MEM_HEIGHT (gl_mem));

  /* find the start of the plane data including padding */
  plane_start =
      gst_gl_get_plane_start (&gl_mem->mem.info, &gl_mem->mem.valign,
      gl_mem->mem.plane) + GST_MEMORY_CAST (gl_mem)->offset;

  gl->BindBuffer (GL_PIXEL_UNPACK_BUFFER, pbo_id);
  gl->BindTexture (gl_target, gl_mem->mem.tex_id);
  gl->TexSubImage2D (gl_target, 0, 0, 0, gl_mem->mem.tex_width,
      GL_MEM_HEIGHT (gl_mem), gl_format, gl_type, (void *) plane_start);
  gl->BindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
  gl->BindTexture (gl_target, 0);

  /* Reset to default values */
  if (USING_OPENGL (context) || USING_GLES3 (context)) {
    gl->PixelStorei (GL_UNPACK_ROW_LENGTH, 0);
  } else if (USING_GLES2 (context)) {
    gl->PixelStorei (GL_UNPACK_ALIGNMENT, 4);
  }
}

static guint
_new_texture (GstGLContext * context, guint target, guint internal_format,
    guint format, guint type, guint width, guint height)
{
  const GstGLFuncs *gl = context->gl_vtable;
  guint tex_id;

  gl->GenTextures (1, &tex_id);
  gl->BindTexture (target, tex_id);
  if (target == GL_TEXTURE_2D || target == GL_TEXTURE_RECTANGLE)
    gl->TexImage2D (target, 0, internal_format, width, height, 0, format, type,
        NULL);

  gl->TexParameteri (target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri (target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri (target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri (target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  gl->BindTexture (target, 0);

  return tex_id;
}

static gboolean
_gl_mem_create (GstGLMemoryPBO * gl_mem, GError ** error)
{
  GstGLContext *context = gl_mem->mem.mem.context;
  GstGLBaseMemoryAllocatorClass *alloc_class;

  alloc_class = GST_GL_BASE_MEMORY_ALLOCATOR_CLASS (parent_class);
  if (!alloc_class->create ((GstGLBaseMemory *) gl_mem, error))
    return FALSE;

  if (USING_OPENGL (context) || USING_OPENGL3 (context)
      || USING_GLES3 (context)) {
    GstAllocationParams params = { 0, GST_MEMORY_CAST (gl_mem)->align, 0, 0 };

    /* FIXME: lazy init this for resource constrained platforms
     * Will need to fix pbo detection based on the existence of the mem.id then */
    gl_mem->pbo = gst_gl_buffer_alloc (context, GL_PIXEL_UNPACK_BUFFER,
        GL_STREAM_DRAW, &params, GST_MEMORY_CAST (gl_mem)->size);
    GST_CAT_LOG (GST_CAT_GL_MEMORY, "generated pbo %u", gl_mem->pbo->id);
  }

  return TRUE;
}

static void
_gl_mem_init (GstGLMemoryPBO * mem, GstAllocator * allocator,
    GstMemory * parent, GstGLContext * context, GstGLTextureTarget target,
    GstAllocationParams * params, GstVideoInfo * info,
    guint plane, GstVideoAlignment * valign, GDestroyNotify notify,
    gpointer user_data)
{
  gst_gl_memory_init ((GstGLMemory *) mem, allocator, parent,
      context, target, params, info, plane, valign, notify, user_data);
}

static GstGLMemoryPBO *
_gl_mem_new (GstAllocator * allocator, GstMemory * parent,
    GstGLContext * context, GstGLTextureTarget target,
    GstAllocationParams * params, GstVideoInfo * info,
    guint plane, GstVideoAlignment * valign, GDestroyNotify notify,
    gpointer user_data)
{
  GstGLMemoryPBO *mem;
  mem = g_slice_new0 (GstGLMemoryPBO);
  mem->mem.texture_wrapped = FALSE;

  _gl_mem_init (mem, allocator, parent, context, target, params, info, plane,
      valign, notify, user_data);

  return mem;
}

static gboolean
_read_pixels_to_pbo (GstGLMemoryPBO * gl_mem)
{
  if (!gl_mem->pbo || !CONTEXT_SUPPORTS_PBO_DOWNLOAD (gl_mem->mem.mem.context)
      || gl_mem->mem.tex_type == GST_VIDEO_GL_TEXTURE_TYPE_LUMINANCE
      || gl_mem->mem.tex_type == GST_VIDEO_GL_TEXTURE_TYPE_LUMINANCE_ALPHA)
    /* unsupported */
    return FALSE;

  if (GST_MEMORY_FLAG_IS_SET (gl_mem,
          GST_GL_BASE_MEMORY_TRANSFER_NEED_DOWNLOAD)) {
    /* copy texture data into into the pbo and map that */
    gsize plane_start;
    GstMapInfo pbo_info;

    plane_start =
        gst_gl_get_plane_start (&gl_mem->mem.info, &gl_mem->mem.valign,
        gl_mem->mem.plane) + GST_MEMORY_CAST (gl_mem)->offset;

    gl_mem->pbo->target = GL_PIXEL_PACK_BUFFER;
    if (!gst_memory_map (GST_MEMORY_CAST (gl_mem->pbo), &pbo_info,
            GST_MAP_WRITE | GST_MAP_GL)) {
      GST_CAT_ERROR (GST_CAT_GL_MEMORY, "Failed to map pbo for writing");
      return FALSE;
    }

    if (!gst_gl_memory_read_pixels ((GstGLMemory *) gl_mem,
            (gpointer) plane_start)) {
      gst_memory_unmap (GST_MEMORY_CAST (gl_mem->pbo), &pbo_info);
      return FALSE;
    }

    gst_memory_unmap (GST_MEMORY_CAST (gl_mem->pbo), &pbo_info);
  }

  return TRUE;
}

static gpointer
_pbo_download_transfer (GstGLMemoryPBO * gl_mem, GstMapInfo * info, gsize size)
{
  GstMapInfo *pbo_info;

  gl_mem->pbo->target = GL_PIXEL_PACK_BUFFER;
  /* texture -> pbo */
  if (info->flags & GST_MAP_READ
      && GST_MEMORY_FLAG_IS_SET (gl_mem,
          GST_GL_BASE_MEMORY_TRANSFER_NEED_DOWNLOAD)) {
    GstMapInfo info;

    GST_CAT_TRACE (GST_CAT_GL_MEMORY,
        "attempting download of texture %u " "using pbo %u", gl_mem->mem.tex_id,
        gl_mem->pbo->id);

    if (!gst_memory_map (GST_MEMORY_CAST (gl_mem->pbo), &info,
            GST_MAP_WRITE | GST_MAP_GL)) {
      GST_CAT_WARNING (GST_CAT_GL_MEMORY, "Failed to write to PBO");
      return NULL;
    }

    if (!_read_pixels_to_pbo (gl_mem))
      return NULL;

    gst_memory_unmap (GST_MEMORY_CAST (gl_mem->pbo), &info);
  }

  pbo_info = g_new0 (GstMapInfo, 1);

  /* pbo -> data */
  /* get a cpu accessible mapping from the pbo */
  if (!gst_memory_map (GST_MEMORY_CAST (gl_mem->pbo), pbo_info, info->flags)) {
    GST_CAT_ERROR (GST_CAT_GL_MEMORY, "Failed to map pbo");
    g_free (pbo_info);
    return NULL;
  }
  info->user_data[0] = pbo_info;

  return pbo_info->data;
}

static gpointer
_gl_mem_map_cpu_access (GstGLMemoryPBO * gl_mem, GstMapInfo * info, gsize size)
{
  gpointer data = NULL;

  gst_gl_base_memory_alloc_data ((GstGLBaseMemory *) gl_mem);

  if (!data && gl_mem->pbo
      && CONTEXT_SUPPORTS_PBO_DOWNLOAD (gl_mem->mem.mem.context))
    data = _pbo_download_transfer (gl_mem, info, size);

  if (!data) {
    GstGLMemoryAllocatorClass *alloc_class;

    alloc_class = GST_GL_MEMORY_ALLOCATOR_CLASS (parent_class);

    data = alloc_class->map ((GstGLBaseMemory *) gl_mem, info, size);
  }

  return data;
}

static gpointer
_gl_mem_map_gpu_access (GstGLMemoryPBO * gl_mem, GstMapInfo * info, gsize size)
{
  gpointer data = &gl_mem->mem.tex_id;

  if ((info->flags & GST_MAP_READ) == GST_MAP_READ) {
    if (gl_mem->pbo && CONTEXT_SUPPORTS_PBO_UPLOAD (gl_mem->mem.mem.context)) {
      GstMapInfo pbo_info;

      /* data -> pbo */
      if (!gst_memory_map (GST_MEMORY_CAST (gl_mem->pbo), &pbo_info,
              GST_MAP_READ | GST_MAP_GL)) {
        GST_CAT_ERROR (GST_CAT_GL_MEMORY, "Failed to map pbo");
        return NULL;
      }

      /* pbo -> texture */
      _upload_pbo_memory (gl_mem, info, gl_mem->pbo, &pbo_info);

      gst_memory_unmap (GST_MEMORY_CAST (gl_mem->pbo), &pbo_info);
    } else {
      GstGLMemoryAllocatorClass *alloc_class;

      alloc_class = GST_GL_MEMORY_ALLOCATOR_CLASS (parent_class);

      data = alloc_class->map ((GstGLBaseMemory *) gl_mem, info, size);
    }
  }

  return data;
}

static gpointer
_gl_mem_map (GstGLMemoryPBO * gl_mem, GstMapInfo * info, gsize maxsize)
{
  gpointer data;

  if ((info->flags & GST_MAP_GL) == GST_MAP_GL) {
    if (gl_mem->mem.tex_target == GST_GL_TEXTURE_TARGET_EXTERNAL_OES)
      return &gl_mem->mem.tex_id;

    data = _gl_mem_map_gpu_access (gl_mem, info, maxsize);
  } else {                      /* not GL */
    if (gl_mem->mem.tex_target == GST_GL_TEXTURE_TARGET_EXTERNAL_OES) {
      GST_CAT_ERROR (GST_CAT_GL_MEMORY, "Cannot map External OES textures");
      return NULL;
    }

    data = _gl_mem_map_cpu_access (gl_mem, info, maxsize);
  }

  return data;
}

static void
_gl_mem_unmap_cpu_access (GstGLMemoryPBO * gl_mem, GstMapInfo * info)
{
  if (!gl_mem->pbo || !CONTEXT_SUPPORTS_PBO_DOWNLOAD (gl_mem->mem.mem.context))
    /* PBO's not supported */
    return;

  gl_mem->pbo->target = GL_PIXEL_PACK_BUFFER;
  gst_memory_unmap (GST_MEMORY_CAST (gl_mem->pbo),
      (GstMapInfo *) info->user_data[0]);
  g_free (info->user_data[0]);
}

static void
_gl_mem_unmap (GstGLMemoryPBO * gl_mem, GstMapInfo * info)
{
  if ((info->flags & GST_MAP_GL) == 0) {
    _gl_mem_unmap_cpu_access (gl_mem, info);
  }
}

static void
_gl_mem_copy_thread (GstGLContext * context, gpointer data)
{
  const GstGLFuncs *gl;
  GstGLMemoryPBOCopyParams *copy_params;
  GstGLMemoryPBO *src;
  guint tex_id;
  guint out_tex_target;
  GLuint fboId;
  gsize out_width, out_height, out_stride;
  GLuint out_gl_format, out_gl_type;
  GLuint in_gl_format, in_gl_type;
  gsize in_size, out_size;

  copy_params = (GstGLMemoryPBOCopyParams *) data;
  src = copy_params->src;
  tex_id = copy_params->tex_id;
  out_tex_target = gst_gl_texture_target_to_gl (copy_params->tex_target);
  out_width = copy_params->out_width;
  out_height = copy_params->out_height;
  out_stride = copy_params->out_stride;

  gl = context->gl_vtable;
  out_gl_format = gst_gl_format_from_gl_texture_type (copy_params->out_format);
  out_gl_type = GL_UNSIGNED_BYTE;
  if (copy_params->out_format == GST_VIDEO_GL_TEXTURE_TYPE_RGB16)
    out_gl_type = GL_UNSIGNED_SHORT_5_6_5;
  in_gl_format = gst_gl_format_from_gl_texture_type (src->mem.tex_type);
  in_gl_type = GL_UNSIGNED_BYTE;
  if (src->mem.tex_type == GST_VIDEO_GL_TEXTURE_TYPE_RGB16)
    in_gl_type = GL_UNSIGNED_SHORT_5_6_5;

  if (!gl->GenFramebuffers) {
    gst_gl_context_set_error (context,
        "Context, EXT_framebuffer_object not supported");
    goto error;
  }

  in_size = GL_MEM_HEIGHT (src) * GL_MEM_STRIDE (src);
  out_size = out_height * out_stride;

  if (copy_params->respecify) {
    if (in_size != out_size) {
      GST_ERROR ("Cannot copy between textures with backing data of different"
          "sizes. input %" G_GSIZE_FORMAT " output %" G_GSIZE_FORMAT, in_size,
          out_size);
      goto error;
    }
  }

  if (!tex_id) {
    guint internal_format;
    guint out_gl_type;

    out_gl_type = GL_UNSIGNED_BYTE;
    if (copy_params->out_format == GST_VIDEO_GL_TEXTURE_TYPE_RGB16)
      out_gl_type = GL_UNSIGNED_SHORT_5_6_5;

    internal_format =
        gst_gl_sized_gl_format_from_gl_format_type (context, out_gl_format,
        out_gl_type);

    tex_id =
        _new_texture (context, out_tex_target,
        internal_format, out_gl_format, out_gl_type, copy_params->out_width,
        copy_params->out_height);
  }

  if (!tex_id) {
    GST_WARNING ("Could not create GL texture with context:%p", context);
  }

  GST_LOG ("copying memory %p, tex %u into texture %i",
      src, src->mem.tex_id, tex_id);

  /* FIXME: try and avoid creating and destroying fbo's every copy... */
  /* create a framebuffer object */
  gl->GenFramebuffers (1, &fboId);
  gl->BindFramebuffer (GL_FRAMEBUFFER, fboId);

  gl->FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      gst_gl_texture_target_to_gl (src->mem.tex_target), src->mem.tex_id, 0);

//  if (!gst_gl_context_check_framebuffer_status (src->mem.mem.context))
//    goto fbo_error;

  gl->BindTexture (out_tex_target, tex_id);
  if (copy_params->respecify) {
    GstMapInfo pbo_info;

    if (!gl->GenBuffers || !src->pbo) {
      gst_gl_context_set_error (context, "Cannot reinterpret texture contents "
          "without pixel buffer objects");
      gl->BindTexture (out_tex_target, 0);
      goto fbo_error;
    }

    if (gst_gl_context_get_gl_api (context) & GST_GL_API_GLES2
        && (in_gl_format != GL_RGBA || in_gl_type != GL_UNSIGNED_BYTE)) {
      gst_gl_context_set_error (context, "Cannot copy non RGBA/UNSIGNED_BYTE "
          "textures on GLES2");
      gl->BindTexture (out_tex_target, 0);
      goto fbo_error;
    }

    GST_TRACE ("copying texture data with size of %u*%u*%u",
        gst_gl_format_type_n_bytes (in_gl_format, in_gl_type),
        src->mem.tex_width, GL_MEM_HEIGHT (src));

    /* copy tex */
    _read_pixels_to_pbo (src);

    src->pbo->target = GL_PIXEL_UNPACK_BUFFER;
    if (!gst_memory_map (GST_MEMORY_CAST (src->pbo), &pbo_info,
            GST_MAP_READ | GST_MAP_GL)) {
      GST_CAT_ERROR (GST_CAT_GL_MEMORY, "Failed to map pbo for reading");
      goto fbo_error;
    }
    gl->TexSubImage2D (out_tex_target, 0, 0, 0, out_width, out_height,
        out_gl_format, out_gl_type, 0);
    gst_memory_unmap (GST_MEMORY_CAST (src->pbo), &pbo_info);
  } else {                      /* different sizes */
    gst_gl_memory_copy_teximage (GST_GL_MEMORY_CAST (src),
        tex_id, copy_params->tex_target, copy_params->out_format, out_width,
        out_height);
  }

  gl->BindTexture (out_tex_target, 0);
  gl->BindFramebuffer (GL_FRAMEBUFFER, 0);

  gl->DeleteFramebuffers (1, &fboId);

  copy_params->tex_id = tex_id;
  copy_params->result = TRUE;

  return;

/* ERRORS */
fbo_error:
  {
    gl->DeleteFramebuffers (1, &fboId);

    copy_params->tex_id = 0;
    copy_params->result = FALSE;
    return;
  }

error:
  {
    copy_params->result = FALSE;
    return;
  }
}

static GstMemory *
_gl_mem_copy (GstGLMemoryPBO * src, gssize offset, gssize size)
{
  GstAllocationParams params = { 0, GST_MEMORY_CAST (src)->align, 0, 0 };
  GstGLBaseMemoryAllocator *base_mem_allocator;
  GstAllocator *allocator;
  GstMemory *dest = NULL;

  allocator = GST_MEMORY_CAST (src)->allocator;
  base_mem_allocator = (GstGLBaseMemoryAllocator *) allocator;

  if (src->mem.tex_target == GST_GL_TEXTURE_TARGET_EXTERNAL_OES) {
    GST_CAT_ERROR (GST_CAT_GL_MEMORY, "Cannot copy External OES textures");
    return NULL;
  }

  /* If not doing a full copy, then copy to sysmem, the 2D represention of the
   * texture would become wrong */
  if (offset > 0 || size < GST_MEMORY_CAST (src)->size) {
    return base_mem_allocator->fallback_mem_copy (GST_MEMORY_CAST (src), offset,
        size);
  }

  dest = (GstMemory *) _gl_mem_new (allocator, NULL, src->mem.mem.context,
      src->mem.tex_target, &params, &src->mem.info, src->mem.plane,
      &src->mem.valign, NULL, NULL);

  if (GST_MEMORY_FLAG_IS_SET (src, GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD)) {
    if (!gst_gl_base_memory_memcpy ((GstGLBaseMemory *) src,
            (GstGLBaseMemory *) dest, offset, size)) {
      GST_CAT_WARNING (GST_CAT_GL_MEMORY, "Could not copy GL Memory");
      gst_memory_unref (GST_MEMORY_CAST (dest));
      return NULL;
    }
  } else {
    GstMapInfo dinfo;

    if (!gst_memory_map (GST_MEMORY_CAST (dest), &dinfo,
            GST_MAP_WRITE | GST_MAP_GL)) {
      GST_CAT_WARNING (GST_CAT_GL_MEMORY,
          "Failed not map destination " "for writing");
      gst_memory_unref (GST_MEMORY_CAST (dest));
      return NULL;
    }

    if (!gst_gl_memory_copy_into ((GstGLMemory *) src,
            ((GstGLMemory *) dest)->tex_id, src->mem.tex_target,
            src->mem.tex_type, src->mem.tex_width, GL_MEM_HEIGHT (src))) {
      GST_CAT_WARNING (GST_CAT_GL_MEMORY, "Could not copy GL Memory");
      gst_memory_unmap (GST_MEMORY_CAST (dest), &dinfo);
      gst_memory_unref (GST_MEMORY_CAST (dest));
      return NULL;
    }

    gst_memory_unmap (GST_MEMORY_CAST (dest), &dinfo);
  }

  return dest;
}

static GstMemory *
_gl_mem_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  g_warning ("Use gst_gl_base_memory_alloc () to allocate from this "
      "GstGLMemoryPBO allocator");

  return NULL;
}

static void
_gl_mem_destroy (GstGLMemoryPBO * gl_mem)
{
  if (gl_mem->pbo)
    gst_memory_unref (GST_MEMORY_CAST (gl_mem->pbo));
  gl_mem->pbo = NULL;

  GST_GL_BASE_MEMORY_ALLOCATOR_CLASS (parent_class)->destroy ((GstGLBaseMemory
          *) gl_mem);
}

static void
_gl_mem_free (GstAllocator * allocator, GstMemory * mem)
{
  GST_ALLOCATOR_CLASS (parent_class)->free (allocator, mem);
}

static GstGLMemoryPBO *
_gl_mem_pbo_alloc (GstGLBaseMemoryAllocator * allocator,
    GstGLVideoAllocationParams * params)
{
  GstGLMemoryPBO *mem;

  g_return_val_if_fail (params->
      parent.alloc_flags & GST_GL_ALLOCATION_PARAMS_ALLOC_FLAG_VIDEO, NULL);

  mem = g_new0 (GstGLMemoryPBO, 1);

  if (params->
      parent.alloc_flags & GST_GL_ALLOCATION_PARAMS_ALLOC_FLAG_WRAP_GPU_HANDLE)
  {
    mem->mem.tex_id = params->parent.gl_handle;
    mem->mem.texture_wrapped = TRUE;
  }

  _gl_mem_init (mem, GST_ALLOCATOR_CAST (allocator), NULL,
      params->parent.context, params->target, params->parent.alloc_params,
      params->v_info, params->plane, params->valign, params->parent.notify,
      params->parent.user_data);

  if (params->
      parent.alloc_flags & GST_GL_ALLOCATION_PARAMS_ALLOC_FLAG_WRAP_GPU_HANDLE)
  {
    GST_MINI_OBJECT_FLAG_SET (mem, GST_GL_BASE_MEMORY_TRANSFER_NEED_DOWNLOAD);
  }
  if (params->
      parent.alloc_flags & GST_GL_ALLOCATION_PARAMS_ALLOC_FLAG_WRAP_SYSMEM) {
    mem->pbo->mem.data = params->parent.wrapped_data;

    GST_MINI_OBJECT_FLAG_SET (mem, GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD);
    GST_MINI_OBJECT_FLAG_SET (mem->pbo,
        GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD);
  }

  return mem;
}

static void
gst_gl_memory_pbo_allocator_class_init (GstGLMemoryPBOAllocatorClass * klass)
{
  GstGLBaseMemoryAllocatorClass *gl_base;
  GstGLMemoryAllocatorClass *gl_tex;
  GstAllocatorClass *allocator_class;

  gl_tex = (GstGLMemoryAllocatorClass *) klass;
  gl_base = (GstGLBaseMemoryAllocatorClass *) klass;
  allocator_class = (GstAllocatorClass *) klass;

  gl_base->alloc = (GstGLBaseMemoryAllocatorAllocFunction) _gl_mem_pbo_alloc;
  gl_base->create = (GstGLBaseMemoryAllocatorCreateFunction) _gl_mem_create;
  gl_tex->map = (GstGLBaseMemoryAllocatorMapFunction) _gl_mem_map;
  gl_tex->unmap = (GstGLBaseMemoryAllocatorUnmapFunction) _gl_mem_unmap;
  gl_tex->copy = (GstGLBaseMemoryAllocatorCopyFunction) _gl_mem_copy;
  gl_base->destroy = (GstGLBaseMemoryAllocatorDestroyFunction) _gl_mem_destroy;

  allocator_class->alloc = _gl_mem_alloc;
  allocator_class->free = _gl_mem_free;
}

static void
gst_gl_memory_pbo_allocator_init (GstGLMemoryPBOAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_GL_MEMORY_PBO_ALLOCATOR_NAME;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

/**
 * gst_gl_memory_pbo_copy_into_texture:
 * @gl_mem:a #GstGLMemoryPBO
 * @tex_id:OpenGL texture id
 * @tex_type: a #GstVideoGLTextureType
 * @width: width of @tex_id
 * @height: height of @tex_id
 * @stride: stride of the backing texture data
 * @respecify: whether to copy the data or copy per texel
 *
 * Copies @gl_mem into the texture specfified by @tex_id.  The format of @tex_id
 * is specified by @tex_type, @width and @height.
 *
 * If @respecify is %TRUE, then the copy is performed in terms of the texture
 * data.  This is useful for splitting RGBA textures into RG or R textures or
 * vice versa. The requirement for this to succeed is that the backing texture
 * data must be the same size, i.e. say a RGBA8 texture is converted into a RG8
 * texture, then the RG texture must have twice as many pixels available for
 * output as the RGBA texture.
 *
 * Otherwise, if @respecify is %FALSE, then the copy is performed per texel
 * using glCopyTexImage.  See the OpenGL specification for details on the
 * mappings between texture formats.
 *
 * Returns: Whether the copy suceeded
 */
gboolean
gst_gl_memory_pbo_copy_into_texture (GstGLMemoryPBO * gl_mem, guint tex_id,
    GstGLTextureTarget target, GstVideoGLTextureType tex_type, gint width,
    gint height, gint stride, gboolean respecify)
{
  GstGLMemoryPBOCopyParams copy_params;

  copy_params.src = gl_mem;
  copy_params.tex_target = target;
  copy_params.tex_id = tex_id;
  copy_params.out_format = tex_type;
  copy_params.out_width = width;
  copy_params.out_height = height;
  copy_params.out_stride = stride;
  copy_params.respecify = respecify;

  gst_gl_context_thread_add (gl_mem->mem.mem.context, _gl_mem_copy_thread,
      &copy_params);

  return copy_params.result;
}

/**
 * gst_gl_memory_pbo_wrapped_texture:
 * @context: a #GstGLContext
 * @texture_id: the GL texture handle
 * @texture_target: the GL texture target
 * @info: the #GstVideoInfo of the memory
 * @plane: The plane this memory will represent
 * @user_data: user data
 * @notify: Destroy callback for the user data
 *
 * Wraps a texture handle into a #GstGLMemoryPBO.
 *
 * Returns: a newly allocated #GstGLMemoryPBO
 */
GstGLMemoryPBO *
gst_gl_memory_pbo_wrapped_texture (GstGLContext * context,
    guint texture_id, GstGLTextureTarget target,
    GstVideoInfo * info, guint plane, GstVideoAlignment * valign,
    gpointer user_data, GDestroyNotify notify)
{
  GstGLMemoryPBO *mem;

  mem = g_slice_new0 (GstGLMemoryPBO);

  mem->mem.tex_id = texture_id;
  mem->mem.texture_wrapped = TRUE;

  _gl_mem_init (mem, _gl_allocator, NULL, context, target, NULL, info, valign,
      plane, user_data, notify);

  GST_MINI_OBJECT_FLAG_SET (mem, GST_GL_BASE_MEMORY_TRANSFER_NEED_DOWNLOAD);

  return mem;
}

/**
 * gst_gl_memory_pbo_alloc:
 * @context:a #GstGLContext
 * @params: a #GstAllocationParams
 * @info: the #GstVideoInfo of the memory
 * @plane: the plane this memory will represent
 * @valign: the #GstVideoAlignment applied to @info
 *
 * Allocated a new #GstGlMemory.
 *
 * Returns: a #GstMemory object with a GL texture specified by @vinfo
 *          from @context
 */
GstMemory *
gst_gl_memory_pbo_alloc (GstGLContext * context, GstGLTextureTarget target,
    GstAllocationParams * params, GstVideoInfo * info, guint plane,
    GstVideoAlignment * valign)
{
  GstGLMemoryPBO *mem;

  mem =
      _gl_mem_new (_gl_allocator, NULL, context, target, params, info, valign,
      plane, NULL, NULL);

  return (GstMemory *) mem;
}

/**
 * gst_gl_memory_pbo_wrapped:
 * @context:a #GstGLContext
 * @info: the #GstVideoInfo of the memory and data
 * @plane: the plane this memory will represent
 * @valign: the #GstVideoAlignment applied to @info
 * @data: the data to wrap
 * @user_data: data called with for @notify
 * @notify: function called with @user_data when @data needs to be freed
 * 
 * Wrapped @data into a #GstGLMemoryPBO. This version will account for padding
 * added to the allocation and expressed through @valign.
 *
 * Returns: a #GstGLMemoryPBO object with a GL texture specified by @v_info
 *          from @context and contents specified by @data
 */
GstGLMemoryPBO *
gst_gl_memory_pbo_wrapped (GstGLContext * context, GstGLTextureTarget target,
    GstVideoInfo * info, guint plane, GstVideoAlignment * valign, gpointer data,
    gpointer user_data, GDestroyNotify notify)
{
  GstGLMemoryPBO *mem;

  mem = _gl_mem_new (_gl_allocator, NULL, context, target, NULL, info, valign,
      plane, user_data, notify);
  if (!mem)
    return NULL;

  mem->pbo->mem.data = data;

  GST_MINI_OBJECT_FLAG_SET (mem, GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD);
  if (mem->pbo)
    GST_MINI_OBJECT_FLAG_SET (mem->pbo,
        GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD);

  return mem;
}

static void
_download_transfer (GstGLContext * context, GstGLMemoryPBO * gl_mem)
{
  GstGLBaseMemory *mem = (GstGLBaseMemory *) gl_mem;

  g_mutex_lock (&mem->lock);
  if (_read_pixels_to_pbo (gl_mem))
    GST_CAT_TRACE (GST_CAT_GL_MEMORY, "optimistic download of texture %u "
        "using pbo %u", gl_mem->mem.tex_id, gl_mem->pbo->id);
  g_mutex_unlock (&mem->lock);
}

void
gst_gl_memory_pbo_download_transfer (GstGLMemoryPBO * gl_mem)
{
  g_return_if_fail (gst_is_gl_memory ((GstMemory *) gl_mem));

  gst_gl_context_thread_add (gl_mem->mem.mem.context,
      (GstGLContextThreadFunc) _download_transfer, gl_mem);
}

static void
_upload_transfer (GstGLContext * context, GstGLMemoryPBO * gl_mem)
{
  GstGLBaseMemory *mem = (GstGLBaseMemory *) gl_mem;
  GstMapInfo info;

  g_mutex_lock (&mem->lock);
  gl_mem->pbo->target = GL_PIXEL_UNPACK_BUFFER;
  if (!gst_memory_map (GST_MEMORY_CAST (gl_mem->pbo), &info,
          GST_MAP_READ | GST_MAP_GL)) {
    GST_CAT_WARNING (GST_CAT_GL_MEMORY, "Failed to map pbo for reading");
  } else {
    gst_memory_unmap (GST_MEMORY_CAST (gl_mem->pbo), &info);
  }
  g_mutex_unlock (&mem->lock);
}

void
gst_gl_memory_pbo_upload_transfer (GstGLMemoryPBO * gl_mem)
{
  g_return_if_fail (gst_is_gl_memory ((GstMemory *) gl_mem));

  if (gl_mem->pbo && CONTEXT_SUPPORTS_PBO_UPLOAD (gl_mem->mem.mem.context))
    gst_gl_context_thread_add (gl_mem->mem.mem.context,
        (GstGLContextThreadFunc) _upload_transfer, gl_mem);
}

/**
 * gst_gl_memory_pbo_init:
 *
 * Initializes the GL Memory allocator. It is safe to call this function
 * multiple times.  This must be called before any other GstGLMemoryPBO operation.
 */
void
gst_gl_memory_pbo_init_once (void)
{
  static volatile gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    gst_gl_memory_init_once ();

    GST_DEBUG_CATEGORY_INIT (GST_CAT_GL_MEMORY, "glmemory", 0, "OpenGL Memory");

    _gl_allocator = g_object_new (GST_TYPE_GL_MEMORY_PBO_ALLOCATOR, NULL);

    gst_allocator_register (GST_GL_MEMORY_PBO_ALLOCATOR_NAME,
        gst_object_ref (_gl_allocator));
    g_once_init_leave (&_init, 1);
  }
}

/**
 * gst_is_gl_memory:
 * @mem:a #GstMemory
 * 
 * Returns: whether the memory at @mem is a #GstGLMemoryPBO
 */
gboolean
gst_is_gl_memory_pbo (GstMemory * mem)
{
  return mem != NULL && mem->allocator != NULL
      && g_type_is_a (G_OBJECT_TYPE (mem->allocator),
      GST_TYPE_GL_MEMORY_PBO_ALLOCATOR);
}

/**
 * gst_gl_memory_pbo_setup_buffer:
 * @context: a #GstGLContext
 * @params: a #GstAllocationParams
 * @info: a #GstVideoInfo
 * @valign: the #GstVideoAlignment applied to @info
 * @buffer: a #GstBuffer
 *
 * Adds the required #GstGLMemoryPBO<!--  -->s with the correct configuration to
 * @buffer based on @info. This version handles padding through @valign.
 *
 * Returns: whether the memory's were sucessfully added.
 */
gboolean
gst_gl_memory_pbo_setup_buffer (GstGLContext * context,
    GstGLTextureTarget target, GstAllocationParams * params,
    GstVideoInfo * info, GstVideoAlignment * valign, GstBuffer * buffer)
{
  GstGLMemoryPBO *gl_mem[GST_VIDEO_MAX_PLANES] = { NULL, };
  guint n_mem, i, v, views;

  n_mem = GST_VIDEO_INFO_N_PLANES (info);

  if (GST_VIDEO_INFO_MULTIVIEW_MODE (info) ==
      GST_VIDEO_MULTIVIEW_MODE_SEPARATED)
    views = info->views;
  else
    views = 1;

  for (v = 0; v < views; v++) {
    for (i = 0; i < n_mem; i++) {
      gl_mem[i] =
          (GstGLMemoryPBO *) gst_gl_memory_pbo_alloc (context, target, params,
          info, i, valign);
      if (gl_mem[i] == NULL)
        return FALSE;

      gst_buffer_append_memory (buffer, (GstMemory *) gl_mem[i]);
    }

    gst_buffer_add_video_meta_full (buffer, v,
        GST_VIDEO_INFO_FORMAT (info), GST_VIDEO_INFO_WIDTH (info),
        GST_VIDEO_INFO_HEIGHT (info), n_mem, info->offset, info->stride);
  }

  return TRUE;
}

/**
 * gst_gl_memory_pbo_setup_wrapped:
 * @context: a #GstGLContext
 * @info: a #GstVideoInfo
 * @valign: a #GstVideoInfo
 * @data: a list of per plane data pointers
 * @textures: (transfer out): a list of #GstGLMemoryPBO
 * @user_data: user data for the destroy function
 * @notify: A function called each time a memory is freed
 *
 * Wraps per plane data pointer in @data into the corresponding entry in
 * @textures based on @info and padding from @valign. Note that the @notify
 * will be called as many time as there is planes.
 *
 * Returns: whether the memory's were sucessfully created.
 */
gboolean
gst_gl_memory_pbo_setup_wrapped (GstGLContext * context,
    GstGLTextureTarget target, GstVideoInfo * info, GstVideoAlignment * valign,
    gpointer data[GST_VIDEO_MAX_PLANES],
    GstGLMemoryPBO * textures[GST_VIDEO_MAX_PLANES], gpointer user_data,
    GDestroyNotify notify)
{
  gint i;

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++) {
    textures[i] =
        (GstGLMemoryPBO *) gst_gl_memory_pbo_wrapped (context, target, info, i,
        valign, data[i], user_data, notify);
  }

  return TRUE;
}