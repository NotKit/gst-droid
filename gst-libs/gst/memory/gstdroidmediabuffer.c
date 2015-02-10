/*
 * gst-droid
 *
 * Copyright (C) 2015 Jolla LTD.
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
#include <config.h>
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gst/gst.h>
#include "gstdroidmediabuffer.h"

GST_DEBUG_CATEGORY_STATIC (droid_memory_debug);
#define GST_CAT_DEFAULT droid_memory_debug

typedef struct
{
  GstAllocator parent;

} GstDroidMediaBufferAllocator;

typedef struct
{
  GstAllocatorClass parent_class;

} GstDroidMediaBufferAllocatorClass;

typedef struct
{
  GstMemory mem;

  DroidMediaBuffer *buffer;

} GstDroidMediaBufferMemory;

#define _do_init \
  GST_DEBUG_CATEGORY_INIT (droid_memory_debug, "droidmemory", 0, \
      "droid memory allocator");

G_DEFINE_TYPE_WITH_CODE (GstDroidMediaBufferAllocator, droid_media_buffer_allocator,
    GST_TYPE_ALLOCATOR, _do_init);

#define GST_TYPE_DROID_MEDIA_BUFFER_ALLOCATOR    (droid_media_buffer_allocator_get_type())
#define GST_IS_DROID_MEDIA_BUFFER_ALLOCATOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_DROID_MEDIA_BUFFER_ALLOCATOR))

static void gst_droid_media_buffer_allocator_free (GstAllocator * allocator,
    GstMemory * mem);

GstAllocator *
gst_droid_media_buffer_allocator_new (void)
{
  return g_object_new (GST_TYPE_DROID_MEDIA_BUFFER_ALLOCATOR, NULL);
}

static void
droid_media_buffer_allocator_init (GstDroidMediaBufferAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  GST_DEBUG_OBJECT (alloc, "init");

  alloc->mem_type = GST_ALLOCATOR_DROID_MEDIA_BUFFER;

  alloc->mem_map = NULL;
  alloc->mem_unmap = NULL;
  alloc->mem_copy = NULL;
  alloc->mem_share = NULL;
  alloc->mem_is_span = NULL;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static void
droid_media_buffer_allocator_class_init (GstDroidMediaBufferAllocatorClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstAllocatorClass *allocator_class = (GstAllocatorClass *) klass;

  allocator_class->alloc = NULL;
  allocator_class->free = gst_droid_media_buffer_allocator_free;
}

GstMemory *
gst_droid_media_buffer_allocator_alloc (GstAllocator * allocator,
					gpointer handle,
					DroidMediaBufferAcquire acquire_func)
{
  GstDroidMediaBufferMemory *mem;
  DroidMediaBuffer *buffer;
  DroidMediaBufferCallbacks cb;

  if (!GST_IS_DROID_MEDIA_BUFFER_ALLOCATOR (allocator)) {
    GST_WARNING_OBJECT (allocator, "allocator is not the correct allocator for droidmediabuffer");
    return NULL;
  }

  mem = g_slice_new0 (GstDroidMediaBufferMemory);

  cb.ref = (void (*)(void *))gst_memory_ref;
  cb.unref = (void (*)(void *))gst_memory_unref;
  cb.data = mem;

  buffer = acquire_func (handle, &cb);
  if (!buffer) {
    GST_ERROR_OBJECT (allocator, "failed to acquire media buffer");
    g_slice_free (GstDroidMediaBufferMemory, mem);
    return NULL;
  }

  mem->buffer = buffer;

  gst_memory_init (GST_MEMORY_CAST (mem),
      GST_MEMORY_FLAG_NO_SHARE | GST_MEMORY_FLAG_NOT_MAPPABLE, allocator, NULL,
      0, 0, 0, 0);

  GST_DEBUG_OBJECT (allocator, "alloc %p", mem);

  return GST_MEMORY_CAST (mem);
}

gboolean
gst_is_droid_media_buffer_memory (GstMemory * mem)
{
  return gst_memory_is_type (mem, GST_ALLOCATOR_DROID_MEDIA_BUFFER);
}

static void
gst_droid_media_buffer_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstDroidMediaBufferMemory *m = (GstDroidMediaBufferMemory *) mem;

  GST_DEBUG_OBJECT (allocator, "free %p", m);

  droid_media_buffer_release (m->buffer, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR);

  m->buffer = NULL;

  g_slice_free (GstDroidMediaBufferMemory, m);
}

DroidMediaBuffer *
gst_droid_media_buffer_memory_get_buffer (GstMemory * mem)
{
  if (!gst_is_droid_media_buffer_memory (mem)) {
    GST_ERROR ("memory %p is not droidmediabuffer memory", mem);
    return NULL;
  }

  return ((GstDroidMediaBufferMemory *)mem)->buffer;
}
