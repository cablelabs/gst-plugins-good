/*
 * Copyright (C) 2014 Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.co.uk>
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
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "gstv4l2videodec.h"
#include "v4l2_calls.h"

#include <string.h>
#include <gst/gst-i18n-plugin.h>

#define DEFAULT_PROP_DEVICE "/dev/video0"

#define V4L2_VIDEO_DEC_QUARK \
	g_quark_from_static_string("gst-v4l2-video-dec-info")

GST_DEBUG_CATEGORY_STATIC (gst_v4l2_video_dec_debug);
#define GST_CAT_DEFAULT gst_v4l2_video_dec_debug

static gboolean gst_v4l2_video_dec_flush (GstVideoDecoder * decoder);

typedef struct
{
  gchar *device;
  GstCaps *sink_caps;
  GstCaps *src_caps;
} Gstv4l2VideoDecQData;

enum
{
  PROP_0,
  V4L2_STD_OBJECT_PROPS,
  PROP_CAPTURE_IO_MODE,
};

static void gst_v4l2_video_dec_class_init (GstV4l2VideoDecClass * klass);
static void gst_v4l2_video_dec_init (GstV4l2VideoDec * self, gpointer g_class);
static void gst_v4l2_video_dec_base_init (gpointer g_class);

static GstVideoDecoderClass *parent_class = NULL;

GType
gst_v4l2_video_dec_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstV4l2VideoDecClass),
      gst_v4l2_video_dec_base_init,
      NULL,
      (GClassInitFunc) gst_v4l2_video_dec_class_init,
      NULL,
      NULL,
      sizeof (GstV4l2VideoDec),
      0,
      (GInstanceInitFunc) gst_v4l2_video_dec_init,
      NULL
    };

    _type = g_type_register_static (GST_TYPE_VIDEO_DECODER, "GstV4l2VideoDec",
        &info, 0);

    g_once_init_leave (&type, _type);
  }
  return type;
}

static void
gst_v4l2_video_dec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  switch (prop_id) {
      /* Split IO mode so output is configure through 'io-mode' and capture
       * through 'capture-io-mode' */
    case PROP_IO_MODE:
      gst_v4l2_object_set_property_helper (self->v4l2output, prop_id, value,
          pspec);
      break;
    case PROP_CAPTURE_IO_MODE:
      gst_v4l2_object_set_property_helper (self->v4l2capture, prop_id, value,
          pspec);
      break;

    case PROP_DEVICE:
      gst_v4l2_object_set_property_helper (self->v4l2output, prop_id, value,
          pspec);
      gst_v4l2_object_set_property_helper (self->v4l2capture, prop_id, value,
          pspec);
      break;

      /* By default, only set on output */
    default:
      if (!gst_v4l2_object_set_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_v4l2_video_dec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  switch (prop_id) {
    case PROP_IO_MODE:
      gst_v4l2_object_get_property_helper (self->v4l2output, prop_id, value,
          pspec);
      break;
    case PROP_CAPTURE_IO_MODE:
      gst_v4l2_object_get_property_helper (self->v4l2output, PROP_IO_MODE,
          value, pspec);
      break;

      /* By default read from output */
    default:
      if (!gst_v4l2_object_get_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static gboolean
gst_v4l2_video_dec_open (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Opening");

  if (!gst_v4l2_object_open (self->v4l2output))
    goto failure;

  if (!gst_v4l2_object_open_shared (self->v4l2capture, self->v4l2output))
    goto failure;

  self->probed_sinkcaps = gst_v4l2_object_get_caps (self->v4l2output,
      gst_v4l2_object_get_codec_caps ());

  if (gst_caps_is_empty (self->probed_sinkcaps))
    goto no_encoded_format;

  self->probed_srccaps = gst_v4l2_object_get_caps (self->v4l2capture,
      gst_v4l2_object_get_raw_caps ());

  if (gst_caps_is_empty (self->probed_srccaps))
    goto no_raw_format;

  return TRUE;

no_encoded_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Encoder on device %s has no supported input format"),
          self->v4l2output->videodev), (NULL));
  goto failure;


no_raw_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Encoder on device %s has no supported output format"),
          self->v4l2output->videodev), (NULL));
  goto failure;

failure:
  if (GST_V4L2_IS_OPEN (self->v4l2output))
    gst_v4l2_object_close (self->v4l2output);

  if (GST_V4L2_IS_OPEN (self->v4l2capture))
    gst_v4l2_object_close (self->v4l2capture);

  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  return FALSE;
}

static gboolean
gst_v4l2_video_dec_close (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing");

  gst_v4l2_object_close (self->v4l2output);
  gst_v4l2_object_close (self->v4l2capture);
  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_start (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting");

  gst_v4l2_object_unlock (self->v4l2output);
  g_atomic_int_set (&self->active, TRUE);
  self->output_flow = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_stop (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  /* Should have been flushed already */
  g_assert (g_atomic_int_get (&self->active) == FALSE);
  g_assert (g_atomic_int_get (&self->processing) == FALSE);

  gst_v4l2_object_stop (self->v4l2output);
  gst_v4l2_object_stop (self->v4l2capture);

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  gboolean ret = TRUE;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Setting format: %" GST_PTR_FORMAT, state->caps);

  if (self->input_state) {
    if (gst_v4l2_object_caps_equal (self->v4l2output, state->caps)) {
      GST_DEBUG_OBJECT (self, "Compatible caps");
      goto done;
    }
    gst_video_codec_state_unref (self->input_state);

    /* FIXME we probably need to do more work if pools are active */
  }

  ret = gst_v4l2_object_set_format (self->v4l2output, state->caps);

  if (ret)
    self->input_state = gst_video_codec_state_ref (state);

done:
  return ret;
}

static gboolean
gst_v4l2_video_dec_flush (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Flushing");

  /* Wait for capture thread to stop */
  gst_pad_stop_task (decoder->srcpad);
  self->output_flow = GST_FLOW_OK;

  gst_v4l2_buffer_pool_flush (GST_V4L2_BUFFER_POOL (self->v4l2output->pool));
  gst_v4l2_buffer_pool_flush (GST_V4L2_BUFFER_POOL (self->v4l2capture->pool));

  /* Output will remain flushing until new frame comes in */
  gst_v4l2_object_unlock_stop (self->v4l2capture);

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_negotiate (GstVideoDecoder * decoder)
{
  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static GstFlowReturn
gst_v4l2_video_dec_finish (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *buffer;

  if (!self->input_state)
    goto done;

  GST_DEBUG_OBJECT (self, "Finishing decoding");

  /* Keep queuing empty buffers until the processing thread has stopped,
   * _pool_process() will return FLUSHING when that happened */
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
  while (ret == GST_FLOW_OK) {
    buffer = gst_buffer_new ();
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->
            v4l2output->pool), buffer);
    gst_buffer_unref (buffer);
  }
  GST_VIDEO_DECODER_STREAM_LOCK (decoder);

  g_assert (g_atomic_int_get (&self->processing) == FALSE);

  if (ret == GST_FLOW_FLUSHING)
    ret = self->output_flow;

  GST_DEBUG_OBJECT (decoder, "Done draining buffers");

done:
  return ret;
}

static GstVideoCodecFrame *
gst_v4l2_video_dec_get_oldest_frame (GstVideoDecoder * decoder)
{
  GstVideoCodecFrame *frame = NULL;
  GList *frames, *l;
  gint count = 0;

  frames = gst_video_decoder_get_frames (decoder);

  for (l = frames; l != NULL; l = l->next) {
    GstVideoCodecFrame *f = l->data;

    if (!frame || frame->pts > f->pts)
      frame = f;

    count++;
  }

  if (frame) {
    GST_LOG_OBJECT (decoder,
        "Oldest frame is %d %" GST_TIME_FORMAT " and %d frames left",
        frame->system_frame_number, GST_TIME_ARGS (frame->pts), count - 1);
    gst_video_codec_frame_ref (frame);
  }

  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);

  return frame;
}

static void
gst_v4l2_video_dec_loop (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstBufferPool *pool;
  GstVideoCodecFrame *frame;
  GstBuffer *buffer = NULL;
  GstFlowReturn ret;

  GST_LOG_OBJECT (decoder, "Allocate output buffer");

  /* We cannot use the base class allotate helper since it taking the internal
   * stream lock. we know that the acquire may need to poll until more frames
   * comes in and holding this lock would prevent that.
   */
  pool = gst_video_decoder_get_buffer_pool (decoder);
  ret = gst_buffer_pool_acquire_buffer (pool, &buffer, NULL);
  g_object_unref (pool);

  if (ret != GST_FLOW_OK)
    goto beach;

  /* Check if buffer isn't the last one */
  if (gst_buffer_get_size (buffer) == 0)
    goto beach;

  GST_LOG_OBJECT (decoder, "Process output buffer");
  ret =
      gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->
          v4l2capture->pool), buffer);

  if (ret != GST_FLOW_OK)
    goto beach;

  frame = gst_v4l2_video_dec_get_oldest_frame (decoder);

  if (frame) {
    frame->output_buffer = buffer;
    buffer = NULL;
    ret = gst_video_decoder_finish_frame (decoder, frame);

    if (ret != GST_FLOW_OK)
      goto beach;
  } else {
    GST_WARNING_OBJECT (decoder, "Decoder is producing too many buffers");
    gst_buffer_unref (buffer);
  }

  return;

beach:
  GST_DEBUG_OBJECT (decoder, "Leaving output thread");

  gst_buffer_replace (&buffer, NULL);
  self->output_flow = ret;
  g_atomic_int_set (&self->processing, FALSE);
  gst_v4l2_object_unlock (self->v4l2output);
  gst_pad_pause_task (decoder->srcpad);
}

static GstFlowReturn
gst_v4l2_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "Handling frame %d", frame->system_frame_number);

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    goto flushing;

  if (G_UNLIKELY (!GST_V4L2_IS_ACTIVE (self->v4l2output))) {
    if (!gst_v4l2_object_set_format (self->v4l2output, self->input_state->caps))
      goto not_negotiated;
  }

  if (G_UNLIKELY (!GST_V4L2_IS_ACTIVE (self->v4l2capture))) {
    GstVideoInfo info;
    GstVideoCodecState *output_state;
    GstBuffer *codec_data;

    GST_DEBUG_OBJECT (self, "Sending header");

    codec_data = self->input_state->codec_data;

    /* We are running in byte-stream mode, so we don't know the headers, but
     * we need to send something, otherwise the decoder will refuse to
     * intialize.
     */
    if (codec_data) {
      gst_buffer_ref (codec_data);
    } else {
      codec_data = frame->input_buffer;
      frame->input_buffer = NULL;
    }

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    gst_v4l2_object_unlock_stop (self->v4l2output);
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->
            v4l2output->pool), codec_data);
    gst_v4l2_object_unlock (self->v4l2output);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    gst_buffer_unref (codec_data);

    if (!gst_v4l2_object_setup_format (self->v4l2capture, &info, &self->align))
      goto not_negotiated;

    output_state = gst_video_decoder_set_output_state (decoder,
        info.finfo->format, info.width, info.height, self->input_state);

    /* Copy the rest of the information, there might be more in the future */
    output_state->info.interlace_mode = info.interlace_mode;
    gst_video_codec_state_unref (output_state);

    if (!gst_video_decoder_negotiate (decoder)) {
      if (GST_PAD_IS_FLUSHING (decoder->srcpad))
        goto flushing;
      else
        goto not_negotiated;
    }
  }

  if (g_atomic_int_get (&self->processing) == FALSE) {
    /* It possible that the processing thread stopped due to an error */
    if (self->output_flow != GST_FLOW_OK) {
      GST_DEBUG_OBJECT (self, "Processing loop stopped with error, leaving");
      ret = self->output_flow;
      goto drop;
    }

    GST_DEBUG_OBJECT (self, "Starting decoding thread");

    /* Enable processing input */
    gst_v4l2_object_unlock_stop (self->v4l2output);

    /* Start the processing task, when it quits, the task will disable input
     * processing to unlock input if draining, or prevent potential block */
    g_atomic_int_set (&self->processing, TRUE);
    gst_pad_start_task (decoder->srcpad,
        (GstTaskFunction) gst_v4l2_video_dec_loop, self, NULL);
  }

  if (frame->input_buffer) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->v4l2output->
            pool), frame->input_buffer);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    if (ret == GST_FLOW_FLUSHING) {
      if (g_atomic_int_get (&self->processing) == FALSE)
        ret = self->output_flow;
    }

    /* No need to keep input arround */
    gst_buffer_replace (&frame->input_buffer, NULL);
  }

  gst_video_codec_frame_unref (frame);
  return ret;

  /* ERRORS */
not_negotiated:
  {
    GST_ERROR_OBJECT (self, "not negotiated");
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto drop;
  }
flushing:
  {
    ret = GST_FLOW_FLUSHING;
    goto drop;
  }
drop:
  {
    gst_video_decoder_drop_frame (decoder, frame);
    return ret;
  }
}

static gboolean
gst_v4l2_video_dec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstClockTime latency;
  gboolean ret = FALSE;

  if (gst_v4l2_object_decide_allocation (self->v4l2capture, query))
    ret = GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (decoder,
        query);

  latency = self->v4l2capture->min_buffers_for_capture *
      self->v4l2capture->duration;
  gst_video_decoder_set_latency (decoder, latency, latency);

  return ret;
}

static gboolean
gst_v4l2_video_dec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *result = NULL;
      gst_query_parse_caps (query, &filter);

      if (self->probed_srccaps)
        result = gst_caps_ref (self->probed_srccaps);
      else
        result = gst_v4l2_object_get_raw_caps ();

      if (filter) {
        GstCaps *tmp = result;
        result =
            gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmp);
      }

      GST_DEBUG_OBJECT (self, "Returning src caps %" GST_PTR_FORMAT, result);

      gst_query_set_caps_result (query, result);
      gst_caps_unref (result);
      break;
    }

    default:
      ret = GST_VIDEO_DECODER_CLASS (parent_class)->src_query (decoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_v4l2_video_dec_sink_query (GstVideoDecoder * decoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *result = NULL;
      gst_query_parse_caps (query, &filter);

      if (self->probed_sinkcaps)
        result = gst_caps_ref (self->probed_sinkcaps);
      else
        result = gst_v4l2_object_get_codec_caps ();

      if (filter) {
        GstCaps *tmp = result;
        result =
            gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmp);
      }

      GST_DEBUG_OBJECT (self, "Returning sink caps %" GST_PTR_FORMAT, result);

      gst_query_set_caps_result (query, result);
      gst_caps_unref (result);
      break;
    }

    default:
      ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_query (decoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_v4l2_video_dec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      gst_v4l2_object_unlock (self->v4l2output);
      gst_v4l2_object_unlock (self->v4l2capture);
    default:
      break;
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);
}

static GstStateChangeReturn
gst_v4l2_video_dec_change_state (GstElement * element,
    GstStateChange transition)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_atomic_int_set (&self->active, FALSE);
    gst_v4l2_object_unlock (self->v4l2output);
    gst_v4l2_object_unlock (self->v4l2capture);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_v4l2_video_dec_dispose (GObject * object)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  gst_caps_replace (&self->probed_sinkcaps, NULL);
  gst_caps_replace (&self->probed_srccaps, NULL);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_v4l2_video_dec_finalize (GObject * object)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  gst_v4l2_object_destroy (self->v4l2capture);
  gst_v4l2_object_destroy (self->v4l2output);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_v4l2_video_dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  Gstv4l2VideoDecQData *qdata;
  GstPadTemplate *templ;

  qdata = g_type_get_qdata (G_TYPE_FROM_CLASS (g_class), V4L2_VIDEO_DEC_QUARK);
  if (!qdata)
    return;

  templ =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
      qdata->sink_caps);
  gst_element_class_add_pad_template (element_class, templ);

  templ =
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
      qdata->src_caps);
  gst_element_class_add_pad_template (element_class, templ);
}

static void
gst_v4l2_video_dec_init (GstV4l2VideoDec * self, gpointer g_class)
{
  GstVideoDecoder *decoder = (GstVideoDecoder *) self;
  Gstv4l2VideoDecQData *qdata;

  qdata = g_type_get_qdata (G_TYPE_FROM_CLASS (g_class), V4L2_VIDEO_DEC_QUARK);
  if (!qdata)
    return;

  gst_video_decoder_set_packetized (decoder, TRUE);

  self->v4l2output = gst_v4l2_object_new (GST_ELEMENT (self),
      V4L2_BUF_TYPE_VIDEO_OUTPUT, qdata->device,
      gst_v4l2_get_output, gst_v4l2_set_output, NULL);
  self->v4l2output->no_initial_format = TRUE;
  self->v4l2output->keep_aspect = FALSE;

  self->v4l2capture = gst_v4l2_object_new (GST_ELEMENT (self),
      V4L2_BUF_TYPE_VIDEO_CAPTURE, qdata->device,
      gst_v4l2_get_input, gst_v4l2_set_input, NULL);
  self->v4l2capture->no_initial_format = TRUE;
  self->v4l2output->keep_aspect = FALSE;

  g_object_set (self, "device", qdata->device, NULL);
}

static void
gst_v4l2_video_dec_class_init (GstV4l2VideoDecClass * klass)
{
  GstElementClass *element_class;
  GObjectClass *gobject_class;
  GstVideoDecoderClass *video_decoder_class;

  parent_class = g_type_class_peek_parent (klass);

  element_class = (GstElementClass *) klass;
  gobject_class = (GObjectClass *) klass;
  video_decoder_class = (GstVideoDecoderClass *) klass;

  gst_element_class_set_static_metadata (element_class,
      "V4L2 Video Decoder",
      "Codec/Decoder/Video",
      "Decode video streams via V4L2 API",
      "Nicolas Dufresne <nicolas.dufresne@collabora.co.uk>");

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_finalize);
  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_get_property);

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_stop);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_finish);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_flush);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_set_format);
  video_decoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_negotiate);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_decide_allocation);
  /* FIXME propose_allocation or not ? */
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_handle_frame);
  video_decoder_class->sink_query =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_sink_query);
  video_decoder_class->src_query =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_src_query);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_sink_event);

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_change_state);

  gst_v4l2_object_install_properties_helper (gobject_class,
      DEFAULT_PROP_DEVICE);

  /**
   * GstV4l2VideoDec:capture-io-mode
   *
   * Capture IO Mode
   */
  g_object_class_install_property (gobject_class, PROP_IO_MODE,
      g_param_spec_enum ("capture-io-mode", "Capture IO mode",
          "Capture I/O mode",
          GST_TYPE_V4L2_IO_MODE, GST_V4L2_IO_AUTO,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

/* Probing functions */
static GstCaps *
gst_v4l2_video_dec_probe_caps (gchar * device, gint video_fd,
    enum v4l2_buf_type type, GstCaps * filter)
{
  gint n;
  struct v4l2_fmtdesc format;
  GstCaps *ret, *caps;

  GST_DEBUG ("Getting %s format enumerations", device);
  caps = gst_caps_new_empty ();

  for (n = 0;; n++) {
    GstStructure *template;

    memset (&format, 0, sizeof (format));

    format.index = n;
    format.type = type;

    if (v4l2_ioctl (video_fd, VIDIOC_ENUM_FMT, &format) < 0)
      break;                    /* end of enumeration */

    GST_LOG ("index:       %u", format.index);
    GST_LOG ("type:        %d", format.type);
    GST_LOG ("flags:       %08x", format.flags);
    GST_LOG ("description: '%s'", format.description);
    GST_LOG ("pixelformat: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (format.pixelformat));

    template = gst_v4l2_object_v4l2fourcc_to_structure (format.pixelformat);

    if (template)
      gst_caps_append_structure (caps, template);
  }

  caps = gst_caps_simplify (caps);

  ret = gst_caps_intersect (filter, caps);
  gst_caps_unref (filter);
  gst_caps_unref (caps);

  return ret;
}

gboolean
gst_v4l2_video_dec_register (GstPlugin * plugin)
{
  gint i = -1;
  gchar *device = NULL;

  GST_DEBUG_CATEGORY_INIT (gst_v4l2_video_dec_debug, "v4l2videodec", 0,
      "V4L2 Video Decoder");

  while (TRUE) {
    GstCaps *src_caps, *sink_caps;
    gint video_fd;

    g_free (device);
    device = g_strdup_printf ("/dev/video%d", ++i);

    if (!g_file_test (device, G_FILE_TEST_EXISTS))
      break;

    video_fd = open (device, O_RDWR);
    if (video_fd == -1) {
      GST_WARNING ("Failed to open %s", device);
      continue;
    }

    /* get sink supported format (no MPLANE for codec) */
    sink_caps = gst_v4l2_video_dec_probe_caps (device, video_fd,
        V4L2_BUF_TYPE_VIDEO_OUTPUT, gst_v4l2_object_get_codec_caps ());

    /* get src supported format */
    src_caps = gst_caps_merge (gst_v4l2_video_dec_probe_caps (device, video_fd,
            V4L2_BUF_TYPE_VIDEO_CAPTURE, gst_v4l2_object_get_raw_caps ()),
        gst_v4l2_video_dec_probe_caps (device, video_fd,
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            gst_v4l2_object_get_raw_caps ()));

    if (!gst_caps_is_empty (sink_caps) && !gst_caps_is_empty (src_caps)) {
      GTypeQuery type_query;
      GTypeInfo type_info = { 0, };
      GType type, subtype;
      gchar *type_name;
      Gstv4l2VideoDecQData *qdata;

      type = gst_v4l2_video_dec_get_type ();
      g_type_query (type, &type_query);
      memset (&type_info, 0, sizeof (type_info));
      type_info.class_size = type_query.class_size;
      type_info.instance_size = type_query.instance_size;

      type_name = g_strdup_printf ("v4l2video%ddec", i);
      subtype = g_type_register_static (type, type_name, &type_info, 0);

      qdata = g_new0 (Gstv4l2VideoDecQData, 1);
      qdata->device = g_strdup (device);
      qdata->sink_caps = gst_caps_ref (sink_caps);
      qdata->src_caps = gst_caps_ref (src_caps);

      g_type_set_qdata (subtype, V4L2_VIDEO_DEC_QUARK, qdata);

      gst_element_register (plugin, type_name, GST_RANK_PRIMARY + 1, subtype);

      g_free (type_name);
    }

    close (video_fd);
    gst_caps_unref (src_caps);
    gst_caps_unref (sink_caps);
  }

  g_free (device);

  return TRUE;
}
