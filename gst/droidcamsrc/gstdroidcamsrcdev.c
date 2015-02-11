/*
 * gst-droid
 *
 * Copyright (C) 2014 Mohammed Sameer <msameer@foolab.org>
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

#include "gstdroidcamsrcdev.h"
#include <stdlib.h>
#include "gstdroidcamsrc.h"
#include "gst/memory/gstdroidmediabuffer.h"
#include "gst/memory/gstwrappedmemory.h"
#include <unistd.h>
#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif /* GST_USE_UNSTABLE_API */
#include <gst/interfaces/photography.h>
#include "gstdroidcamsrcexif.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

GST_DEBUG_CATEGORY_EXTERN (gst_droid_camsrc_debug);
#define GST_CAT_DEFAULT gst_droid_camsrc_debug

#define VIDEO_RECORDING_STOP_TIMEOUT                 100000     /* us */

struct _GstDroidCamSrcImageCaptureState
{
  gboolean image_preview_sent;
  gboolean image_start_sent;
};

struct _GstDroidCamSrcVideoCaptureState
{
  unsigned long video_frames;
  int queued_frames;
  gboolean running;
  gboolean eos_sent;
  GMutex lock;
};

typedef struct _GstDroidCamSrcDevVideoData
{
  GstDroidCamSrcDev *dev;
  DroidMediaCameraRecordingData *data;
} GstDroidCamSrcDevVideoData;

static void gst_droidcamsrc_dev_release_recording_frame (void *data,
    GstDroidCamSrcDevVideoData *video_data);
static void gst_droidcamsrc_dev_release_preview_frame (DroidMediaBuffer *buffer,
    GstDroidCamSrcDev *dev);
void gst_droidcamsrc_dev_update_params_locked (GstDroidCamSrcDev * dev);

static void
gst_droidcamsrc_dev_notify_callback (void *user, int32_t msg_type,
    int32_t ext1, int32_t ext2)
{
  GstDroidCamSrcDev *dev = (GstDroidCamSrcDev *) user;
  GstDroidCamSrc *src = GST_DROIDCAMSRC (GST_PAD_PARENT (dev->imgsrc->pad));

  GST_DEBUG_OBJECT (src, "dev notify callback");

  // TODO: more messages

  switch (msg_type) {
    case CAMERA_MSG_SHUTTER:
      g_rec_mutex_lock (dev->lock);
      if (!dev->img->image_start_sent) {
        gst_droidcamsrc_post_message (src,
            gst_structure_new_empty (GST_DROIDCAMSRC_CAPTURE_START));
        dev->img->image_start_sent = TRUE;
      }
      g_rec_mutex_unlock (dev->lock);
      break;

    case CAMERA_MSG_FOCUS:
    {
      GstStructure *s;
      gint status;
      if (ext1) {
        status = GST_PHOTOGRAPHY_FOCUS_STATUS_SUCCESS;
      } else {
        status = GST_PHOTOGRAPHY_FOCUS_STATUS_FAIL;
      }

      s = gst_structure_new (GST_PHOTOGRAPHY_AUTOFOCUS_DONE, "status",
          G_TYPE_INT, status, NULL);
      gst_droidcamsrc_post_message (src, s);
    }

      break;

    case CAMERA_MSG_FOCUS_MOVE:
    {
      // TODO: an idea could be to query focus state when moving stops or starts
      // and use that to emulate realtime reporting of CAF status
      GstStructure *s;
      GST_LOG_OBJECT (src, "focus move %d", ext1);

      s = gst_structure_new ("focus-move", "status", G_TYPE_INT, ext1, NULL);
      gst_droidcamsrc_post_message (src, s);
    }

      break;

    case CAMERA_MSG_ERROR:
      GST_ELEMENT_ERROR (src, LIBRARY, FAILED, (NULL),
          ("error from camera HAL with args 0x%x 0x%x", ext1, ext2));
      break;

    default:
      GST_WARNING_OBJECT (src, "unknown message type 0x%x", msg_type);
  }
}

static void
gst_droidcamsrc_dev_data_callback (void *user, int32_t msg_type, DroidMediaData *mem)
//    camera_frame_metadata_t * metadata, void *user)
{
  GstDroidCamSrcDev *dev = (GstDroidCamSrcDev *) user;
  GstDroidCamSrc *src = GST_DROIDCAMSRC (GST_PAD_PARENT (dev->imgsrc->pad));

  GST_DEBUG_OBJECT (src, "dev data callback");

  switch (msg_type) {
    case CAMERA_MSG_RAW_IMAGE:
      // TODO:
      break;

    case CAMERA_MSG_COMPRESSED_IMAGE:
    {
      size_t size = mem->size;
      void *addr = mem->data;
      if (!addr) {
        GST_ERROR_OBJECT (src, "invalid memory from camera hal");
      } else {
        GstBuffer *buffer;
        GstTagList *tags;
        GstEvent *event = NULL;
        void *d = g_malloc (size);
        memcpy (d, addr, size);
        buffer = gst_buffer_new_wrapped (d, size);
        if (!dev->img->image_preview_sent) {
          gst_droidcamsrc_post_message (src,
              gst_structure_new_empty (GST_DROIDCAMSRC_CAPTURE_END));
          // TODO: generate and send preview.
          dev->img->image_preview_sent = TRUE;
        }

        gst_droidcamsrc_timestamp (src, buffer);

        tags = gst_droidcamsrc_exif_tags_from_jpeg_data (d, size);
        if (tags) {
          GST_INFO_OBJECT (src, "pushing tags %" GST_PTR_FORMAT, tags);
          event = gst_event_new_tag (tags);
        }

        g_mutex_lock (&dev->imgsrc->queue_lock);

        if (event) {
          src->imgsrc->pending_events =
              g_list_append (src->imgsrc->pending_events, event);
        }

        g_queue_push_tail (dev->imgsrc->queue, buffer);
        g_cond_signal (&dev->imgsrc->cond);
        g_mutex_unlock (&dev->imgsrc->queue_lock);
      }

      /* we need to start restart the preview
       * android demands this but GStreamer does not know about it.
       */
      g_rec_mutex_lock (dev->lock);
      dev->running = FALSE;
      g_rec_mutex_unlock (dev->lock);

      gst_droidcamsrc_dev_start (dev, TRUE);

      g_mutex_lock (&src->capture_lock);
      --src->captures;
      g_mutex_unlock (&src->capture_lock);

      g_object_notify (G_OBJECT (src), "ready-for-capture");
    }
      break;
#if 0
    case CAMERA_MSG_PREVIEW_METADATA:
    {
      int i;
      GstStructure *s;
      int width = 0, height = 0;
      GValue regions = G_VALUE_INIT;

      GST_INFO_OBJECT (src, "camera detected %d faces",
          metadata->number_of_faces);
      /*
       * It should be safe to access window here
       * We cannot really take dev->lock otherwise we might deadlock
       * if we happen to try to acquire it while the device is being stopped.
       * window gets destroyed when we destroy the whole device so it is
       * not going anywhere.
       */
      if (dev->win) {
        g_mutex_lock (&dev->win->lock);
        width = dev->win->width;
        height = dev->win->height;
        g_mutex_unlock (&dev->win->lock);
      }

      if (!width || !height) {
        GST_WARNING_OBJECT (src, "failed to get preview dimensions");
        return;
      }

      s = gst_structure_new ("regions-of-interest", "frame-width", G_TYPE_UINT,
          width, "frame-height", G_TYPE_UINT, height, NULL);

      g_value_init (&regions, GST_TYPE_LIST);

      for (i = 0; i < metadata->number_of_faces; i++) {
        GValue region = G_VALUE_INIT;
        int x, y, w, h, r, b;
        GstStructure *rs;

        g_value_init (&region, GST_TYPE_STRUCTURE);

        GST_DEBUG_OBJECT (src,
            "face %d: left = %d, top = %d, right = %d, bottom = %d", i,
            metadata->faces->rect[0], metadata->faces->rect[1],
            metadata->faces->rect[2], metadata->faces->rect[3]);
        x = gst_util_uint64_scale (metadata->faces[i].rect[0] + 1000, width,
            2000);
        y = gst_util_uint64_scale (metadata->faces[i].rect[1] + 1000, height,
            2000);
        r = gst_util_uint64_scale (metadata->faces[i].rect[2] + 1000, width,
            2000);
        b = gst_util_uint64_scale (metadata->faces[i].rect[3] + 1000, height,
            2000);
        w = r - x;
        h = b - y;
        rs = gst_structure_new ("region-of-interest",
            "region-x", G_TYPE_UINT, x,
            "region-y", G_TYPE_UINT, y,
            "region-w", G_TYPE_UINT, w,
            "region-h", G_TYPE_UINT, h,
            "region-id", G_TYPE_INT, metadata->faces[i].id,
            "region-score", G_TYPE_INT, metadata->faces[i].score, NULL);

        gst_value_set_structure (&region, rs);
        gst_structure_free (rs);
        gst_value_list_append_value (&regions, &region);
        g_value_unset (&region);
      }

      gst_structure_take_value (s, "regions", &regions);
      gst_droidcamsrc_post_message (src, s);
    }
      break;
#endif
    case CAMERA_MSG_PREVIEW_FRAME:
      GST_LOG_OBJECT (src, "dropping preview frame message");
      break;

    default:
      GST_WARNING_OBJECT (src, "unknown message type 0x%x", msg_type);
  }

  // TODO:
}

static void
gst_droidcamsrc_dev_data_timestamp_callback (void *user,
    int32_t msg_type, DroidMediaCameraRecordingData *data)
{
  void *addr = droid_media_camera_recording_frame_get_data (data);
  gboolean drop_buffer;
  GstBuffer *buffer;
  GstMemory *mem;
  GstDroidCamSrcDev *dev = (GstDroidCamSrcDev *) user;
  GstDroidCamSrc *src = GST_DROIDCAMSRC (GST_PAD_PARENT (dev->imgsrc->pad));
  GstDroidCamSrcDevVideoData *video_data;

  g_mutex_lock (&dev->vid->lock);

  // TODO: not sure what to do with timestamp

  GST_DEBUG_OBJECT (src, "dev data timestamp callback");

  /* unlikely but just in case */
  if (msg_type != CAMERA_MSG_VIDEO_FRAME) {
    GST_ERROR ("unknown message type 0x%x", msg_type);
    droid_media_camera_release_recording_frame (dev->cam, data);
    goto unlock_and_out;
  }

  if (!addr) {
    GST_ERROR ("invalid memory from camera HAL");
    droid_media_camera_release_recording_frame (dev->cam, data);
    goto unlock_and_out;
  }

  // TODO:
  video_data = g_slice_new0(GstDroidCamSrcDevVideoData);
  video_data->dev = dev;
  video_data->data = data;

  buffer = gst_buffer_new ();
  mem = gst_wrapped_memory_allocator_wrap (dev->wrap_allocator,
      addr, droid_media_camera_recording_frame_get_size (data),
     (GFunc) gst_droidcamsrc_dev_release_recording_frame, video_data);
  gst_buffer_insert_memory (buffer, 0, mem);

  GST_BUFFER_OFFSET (buffer) = dev->vid->video_frames;
  GST_BUFFER_OFFSET_END (buffer) = ++dev->vid->video_frames;
  gst_droidcamsrc_timestamp (src, buffer);

  g_rec_mutex_lock (dev->lock);

  drop_buffer = !dev->vid->running;
  if (!drop_buffer) {
    ++dev->vid->queued_frames;
  }

  g_rec_mutex_unlock (dev->lock);

  if (drop_buffer) {
    GST_INFO_OBJECT (src, "dropping buffer because video recording is not running");
    gst_buffer_unref (buffer);
  } else {
    g_mutex_lock (&dev->vidsrc->queue_lock);
    g_queue_push_tail (dev->vidsrc->queue, buffer);
    g_cond_signal (&dev->vidsrc->cond);
    g_mutex_unlock (&dev->vidsrc->queue_lock);
  }

unlock_and_out:
  g_mutex_unlock (&dev->vid->lock);
}

static void
gst_droidcamsrc_dev_buffers_released(G_GNUC_UNUSED void *user)
{
  GstDroidCamSrcDev *dev = (GstDroidCamSrcDev *) user;

  GST_FIXME_OBJECT (dev, "Not sure what to do here really");
}

static void
gst_droidcamsrc_dev_frame_available(void *user)
{
  GstDroidCamSrcDev *dev = (GstDroidCamSrcDev *) user;
  GstDroidCamSrc *src = GST_DROIDCAMSRC (GST_PAD_PARENT (dev->imgsrc->pad));
  GstDroidCamSrcPad *pad = dev->vfsrc;
  DroidMediaBuffer *buffer;
  GstMemory *mem;
  GstVideoCropMeta *crop_meta;
  DroidMediaRect rect;
  guint width, height;
  GstBuffer *buff;

  GST_DEBUG_OBJECT (src, "frame available");

  if (!pad->running) {
    GST_DEBUG_OBJECT (src, "vfsrc pad task is not running");
    return;
  }

  /* TODO: size */
  mem = gst_droid_media_buffer_allocator_alloc (dev->media_allocator, dev->cam,
						(DroidMediaBufferAcquire) droid_media_camera_acquire_buffer);

  if (!mem) {
    GST_ERROR_OBJECT (src, "failed to acquire buffer from droidmedia");
    return;
  }

  buffer = gst_droid_media_buffer_memory_get_buffer (mem);

  buff = gst_buffer_new ();

  gst_buffer_insert_memory (buff, 0, mem);
  gst_droidcamsrc_timestamp (src, buff);

  rect = droid_media_buffer_get_crop_rect (buffer);
  crop_meta = gst_buffer_add_video_crop_meta (buff);
  crop_meta->x = rect.left;
  crop_meta->y = rect.top;
  crop_meta->width = rect.right - rect.left;
  crop_meta->height = rect.bottom - rect.top;

  gst_buffer_add_gst_buffer_orientation_meta (buff,
      dev->info->orientation, dev->info->direction);

  width = droid_media_buffer_get_width(buffer);
  height = droid_media_buffer_get_height(buffer);

  gst_buffer_add_video_meta (buff, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_YV12,
			     width, height);

  GST_LOG_OBJECT (src, "preview info: w=%d, h=%d, crop: x=%d, y=%d, w=%d, h=%d", width, height, crop_meta->x,
		  crop_meta->y, crop_meta->width, crop_meta->height);

  g_mutex_lock (&pad->queue_lock);

  g_queue_push_tail (pad->queue, buff);

  g_cond_signal (&pad->cond);

  g_mutex_unlock (&pad->queue_lock);
}

GstDroidCamSrcDev *
gst_droidcamsrc_dev_new (GstDroidCamSrcPad * vfsrc,
    GstDroidCamSrcPad * imgsrc, GstDroidCamSrcPad * vidsrc, GRecMutex * lock)
{
  GstDroidCamSrcDev *dev;

  GST_DEBUG ("dev new");

  dev = g_slice_new0 (GstDroidCamSrcDev);
  dev->cam = NULL;
  dev->running = FALSE;
  dev->info = NULL;
  dev->img = g_slice_new0 (GstDroidCamSrcImageCaptureState);
  dev->vid = g_slice_new0 (GstDroidCamSrcVideoCaptureState);

  g_mutex_init (&dev->vid->lock);

  dev->wrap_allocator = gst_wrapped_memory_allocator_new ();
  dev->media_allocator = gst_droid_media_buffer_allocator_new ();
  dev->vfsrc = vfsrc;
  dev->imgsrc = imgsrc;
  dev->vidsrc = vidsrc;

  dev->lock = lock;

  return dev;
}

gboolean
gst_droidcamsrc_dev_open (GstDroidCamSrcDev * dev, GstDroidCamSrcCamInfo * info)
{
  GstDroidCamSrc *src;

  g_rec_mutex_lock (dev->lock);

  src = GST_DROIDCAMSRC (GST_PAD_PARENT (dev->imgsrc->pad));

  GST_DEBUG_OBJECT (src, "dev open");

  dev->info = info;
  dev->cam = droid_media_camera_connect (dev->info->num);

  if (!dev->cam) {
    g_rec_mutex_unlock (dev->lock);

    GST_ELEMENT_ERROR (src, LIBRARY, INIT, (NULL), ("error opening camera"));
    return FALSE;
  }

  if (!droid_media_camera_lock (dev->cam)) {
    droid_media_camera_disconnect(dev->cam);
    dev->cam = NULL;

    GST_ELEMENT_ERROR (src, LIBRARY, INIT, (NULL), ("error locking camera"));
    return FALSE;
  }

  /* disable shutter sound */
  droid_media_camera_send_command (dev->cam, CAMERA_CMD_ENABLE_SHUTTER_SOUND, 0, 0);

  g_rec_mutex_unlock (dev->lock);

  return TRUE;
}

void
gst_droidcamsrc_dev_close (GstDroidCamSrcDev * dev)
{
  GST_DEBUG ("dev close");

  g_rec_mutex_lock (dev->lock);

  if (dev->cam) {
    if (!droid_media_camera_unlock (dev->cam)) {
      GST_ERROR ("error unlocking camera");
    }

    droid_media_camera_disconnect (dev->cam);
    dev->cam = NULL;
  }

  g_rec_mutex_unlock (dev->lock);
}

void
gst_droidcamsrc_dev_destroy (GstDroidCamSrcDev * dev)
{
  GST_DEBUG ("dev destroy");

  dev->cam = NULL;
  dev->info = NULL;
  gst_object_unref (dev->wrap_allocator);
  dev->wrap_allocator = NULL;

  gst_object_unref (dev->media_allocator);
  dev->media_allocator = NULL;

  g_mutex_clear (&dev->vid->lock);

  g_slice_free (GstDroidCamSrcImageCaptureState, dev->img);
  g_slice_free (GstDroidCamSrcVideoCaptureState, dev->vid);
  g_slice_free (GstDroidCamSrcDev, dev);
  dev = NULL;
}

gboolean
gst_droidcamsrc_dev_init (GstDroidCamSrcDev * dev)
{
  GST_DEBUG ("dev init");

  g_rec_mutex_lock (dev->lock);

  {
    DroidMediaCameraCallbacks cb;

    cb.notify = gst_droidcamsrc_dev_notify_callback;
    cb.post_data_timestamp = gst_droidcamsrc_dev_data_timestamp_callback;
    cb.post_data = gst_droidcamsrc_dev_data_callback;
    droid_media_camera_set_callbacks (dev->cam, &cb, dev);
  }

  {
    DroidMediaRenderingCallbacks cb;
    cb.buffers_released = gst_droidcamsrc_dev_buffers_released;
    cb.frame_available = gst_droidcamsrc_dev_frame_available;
    droid_media_camera_set_rendering_callbacks (dev->cam, &cb, dev);
  }

  gst_droidcamsrc_dev_update_params_locked (dev);

  g_rec_mutex_unlock (dev->lock);

  return TRUE;
}

void
gst_droidcamsrc_dev_deinit (GstDroidCamSrcDev * dev)
{
  GST_DEBUG ("dev deinit");

  g_rec_mutex_lock (dev->lock);

  if (dev->params) {
    gst_droidcamsrc_params_destroy (dev->params);
    dev->params = NULL;
  }

  g_rec_mutex_unlock (dev->lock);
}

gboolean
gst_droidcamsrc_dev_start (GstDroidCamSrcDev * dev, gboolean apply_settings)
{
  gboolean ret = FALSE;
  GstDroidCamSrc *src = GST_DROIDCAMSRC (GST_PAD_PARENT (dev->imgsrc->pad));

  g_rec_mutex_lock (dev->lock);

  if (dev->running) {
    GST_WARNING_OBJECT (src, "preview is already running");
    ret = TRUE;
    goto out;
  }

  GST_DEBUG_OBJECT (src, "dev start");

  if (apply_settings) {
    gst_droidcamsrc_apply_mode_settings (src, SET_ONLY);
  }

  /* now set params */
  if (!gst_droidcamsrc_dev_set_params (dev)) {
    goto out;
  }

  /* We don't want the preview frame. We will render it using the GraphicBuffers we get */
  droid_media_camera_set_preview_callback_flags(dev->cam, CAMERA_FRAME_CALLBACK_FLAG_NOOP);
  if (!droid_media_camera_start_preview (dev->cam)) {
    GST_ERROR_OBJECT (src, "error starting preview");
    goto out;
  }

  dev->running = TRUE;

  ret = TRUE;

out:
  g_rec_mutex_unlock (dev->lock);
  return ret;
}

void
gst_droidcamsrc_dev_stop (GstDroidCamSrcDev * dev)
{
  g_rec_mutex_lock (dev->lock);

  GST_DEBUG ("dev stop");

  if (dev->running) {
    GST_DEBUG ("stopping preview");
    droid_media_camera_stop_preview (dev->cam);
    dev->running = FALSE;
    GST_DEBUG ("stopped preview");
  }

  g_rec_mutex_unlock (dev->lock);
}

gboolean
gst_droidcamsrc_dev_set_params (GstDroidCamSrcDev * dev)
{
  bool err;
  gboolean ret = FALSE;
  gchar *params;

  g_rec_mutex_lock (dev->lock);
  if (!dev->cam) {
    GST_ERROR ("camera device is not open");
    goto out;
  }

  if (!dev->params) {
    GST_ERROR ("camera device is not initialized");
    goto out;
  }

  if (!gst_droidcamsrc_params_is_dirty (dev->params)) {
    GST_DEBUG ("no need to reset params");
    ret = TRUE;
    goto out;
  }

  params = gst_droidcamsrc_params_to_string (dev->params);
  GST_LOG ("setting parameters %s", params);
  err = droid_media_camera_set_parameters (dev->cam, params);
  g_free (params);

  if (!err) {
    GST_ERROR ("error setting parameters");
    goto out;
  }

  ret = TRUE;

out:
  g_rec_mutex_unlock (dev->lock);

  return ret;
}

gboolean
gst_droidcamsrc_dev_capture_image (GstDroidCamSrcDev * dev)
{
  gboolean ret = FALSE;
  int msg_type = CAMERA_MSG_SHUTTER | CAMERA_MSG_RAW_IMAGE
    | CAMERA_MSG_POSTVIEW_FRAME | CAMERA_MSG_COMPRESSED_IMAGE;

  GST_DEBUG ("dev capture image");

  g_rec_mutex_lock (dev->lock);

  dev->img->image_preview_sent = FALSE;
  dev->img->image_start_sent = FALSE;

  if (!droid_media_camera_take_picture (dev->cam, msg_type)) {
    GST_ERROR ("error capturing image");
    goto out;
  }

  ret = TRUE;

out:
  g_rec_mutex_unlock (dev->lock);
  return ret;
}

gboolean
gst_droidcamsrc_dev_start_video_recording (GstDroidCamSrcDev * dev)
{
  gboolean ret = FALSE;

  GST_DEBUG ("dev start video recording");

  g_mutex_lock (&dev->vidsrc->queue_lock);
  dev->vidsrc->pushed_buffers = 0;
  g_mutex_unlock (&dev->vidsrc->queue_lock);

  g_rec_mutex_lock (dev->lock);
  dev->vid->running = TRUE;
  dev->vid->eos_sent = FALSE;
  dev->vid->video_frames = 0;
  dev->vid->queued_frames = 0;

  // TODO: get that from caps
  if (!droid_media_camera_store_meta_data_in_buffers (dev->cam, true)) {
    GST_ERROR ("error storing meta data in buffers for video recording");
    goto out;
  }

  if (!droid_media_camera_start_recording (dev->cam)) {
    GST_ERROR ("error starting video recording");
    goto out;
  }

  ret = TRUE;

out:
  g_rec_mutex_unlock (dev->lock);
  return ret;
}

void
gst_droidcamsrc_dev_stop_video_recording (GstDroidCamSrcDev * dev)
{
  GST_DEBUG ("dev stop video recording");

  // TODO: review all those locks
  /* We need to make sure that some buffers have been pushed */
  g_mutex_lock (&dev->vidsrc->queue_lock);
  while (dev->vid->video_frames <= 4) {
    g_mutex_unlock (&dev->vidsrc->queue_lock);
    usleep (30000);             // TODO: bad
    g_mutex_lock (&dev->vidsrc->queue_lock);
  }

  g_mutex_unlock (&dev->vidsrc->queue_lock);

  /* Now stop pushing to the pad */
  g_rec_mutex_lock (dev->lock);
  dev->vid->running = FALSE;
  g_rec_mutex_unlock (dev->lock);

  /* now make sure nothing is being pushed to the queue */
  g_mutex_lock (&dev->vid->lock);
  g_mutex_unlock (&dev->vid->lock);

  /* our pad task is either sleeping or still pushing buffers. We empty the queue. */
  g_mutex_lock (&dev->vidsrc->queue_lock);
  g_queue_foreach (dev->vidsrc->queue, (GFunc) gst_buffer_unref, NULL);
  g_mutex_unlock (&dev->vidsrc->queue_lock);

  /* now we are done. We just push eos */
  GST_DEBUG ("Pushing EOS");
  if (!gst_pad_push_event (dev->vidsrc->pad, gst_event_new_eos ())) {
    GST_ERROR ("failed to push EOS event");
  }

  g_rec_mutex_lock (dev->lock);

  GST_INFO ("waiting for queued frames %i", dev->vid->queued_frames);

  if (dev->vid->queued_frames > 0) {
    GST_INFO ("waiting for queued frames to reach 0 from %i",
        dev->vid->queued_frames);
    g_rec_mutex_unlock (dev->lock);
    usleep (VIDEO_RECORDING_STOP_TIMEOUT);
    g_rec_mutex_lock (dev->lock);
  }

  if (dev->vid->queued_frames > 0) {
    GST_WARNING ("video queue still has %i frames", dev->vid->queued_frames);
  }

  g_rec_mutex_unlock (dev->lock);

  droid_media_camera_stop_recording (dev->cam);

  GST_INFO ("dev stopped video recording");
}

static void
gst_droidcamsrc_dev_release_recording_frame (void *data,
    GstDroidCamSrcDevVideoData *video_data)
{
  GstDroidCamSrcDev *dev;

  GST_DEBUG ("dev release recording frame %p", data);

  dev = video_data->dev;

  g_rec_mutex_lock (dev->lock);
  --dev->vid->queued_frames;

  droid_media_camera_release_recording_frame (dev->cam, video_data->data);

  g_slice_free(GstDroidCamSrcDevVideoData, video_data);
  g_rec_mutex_unlock (dev->lock);
}

void
gst_droidcamsrc_dev_update_params_locked (GstDroidCamSrcDev * dev)
{
  gchar *params;

  params = droid_media_camera_get_parameters (dev->cam);

  if (dev->params) {
    // TODO: is this really needed? We might lose some unset params if we do that.
    gst_droidcamsrc_params_reload (dev->params, params);
  } else {
    dev->params = gst_droidcamsrc_params_new (params);
  }

  free (params);
}

void
gst_droidcamsrc_dev_update_params (GstDroidCamSrcDev * dev)
{
  g_rec_mutex_lock (dev->lock);
  gst_droidcamsrc_dev_update_params_locked (dev);
  g_rec_mutex_unlock (dev->lock);
}

gboolean
gst_droidcamsrc_dev_start_autofocus (GstDroidCamSrcDev * dev)
{
  gboolean ret = FALSE;

  g_rec_mutex_lock (dev->lock);

  if (!dev->cam) {
    GST_WARNING ("cannot autofocus because camera is not running");
    goto out;
  }

  if (!droid_media_camera_start_auto_focus (dev->cam)) {
    GST_WARNING ("error starting autofocus");
    goto out;
  }

  ret = TRUE;

out:
  g_rec_mutex_unlock (dev->lock);

  return ret;
}

void
gst_droidcamsrc_dev_stop_autofocus (GstDroidCamSrcDev * dev)
{
  g_rec_mutex_lock (dev->lock);

  if (dev->cam) {
    if (!droid_media_camera_cancel_auto_focus (dev->cam))
      GST_WARNING ("error stopping autofocus");
  }

  g_rec_mutex_unlock (dev->lock);
}

// TODO: the name is not descriptive to what the function does.
gboolean
gst_droidcamsrc_dev_enable_face_detection (GstDroidCamSrcDev * dev,
    gboolean enable)
{
  int32_t cmd;

  gboolean res = FALSE;

  GST_LOG ("enable face detection %d", enable);

  cmd =
      enable ? CAMERA_CMD_START_FACE_DETECTION : CAMERA_CMD_STOP_FACE_DETECTION;

  g_rec_mutex_lock (dev->lock);
  if (!dev->cam) {
    GST_WARNING ("camera is not running yet");
    goto out;
  }

  // TODO: this is SW only. We need to investigate HW too.
  if (!droid_media_camera_send_command (dev->cam, cmd, CAMERA_FACE_DETECTION_SW, 0)) {
    GST_ERROR ("error enabling face detection");
    goto out;
  }

  res = TRUE;

out:
  g_rec_mutex_unlock (dev->lock);

  return res;
}

gboolean
gst_droidcamsrc_dev_restart (GstDroidCamSrcDev * dev)
{
  gboolean ret = FALSE;

  g_rec_mutex_lock (dev->lock);

  GST_DEBUG ("dev restart");

  if (dev->running) {
    gst_droidcamsrc_dev_stop (dev);
    ret = gst_droidcamsrc_dev_start (dev, TRUE);
  } else {
    ret = TRUE;
  }

  g_rec_mutex_unlock (dev->lock);

  return ret;
}
