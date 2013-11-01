/* GStreamer
 * Copyright (C) 2013 Cable Television Labs, Inc. <info@cablelabs.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstsouphttpserversink
 *
 * The souphttpserversink element serves a stream via HTTP.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v videotestsrc ! x264enc ! mpegtsmux !
 *     souphttpserversink port=8080
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include "gstsouphttpserversink.h"

#include <gst/glib-compat-private.h>

GST_DEBUG_CATEGORY_STATIC (souphttpserversink_dbg);
#define GST_CAT_DEFAULT souphttpserversink_dbg

/* prototypes */

static void gst_soup_http_client_sink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_soup_http_client_sink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_soup_http_server_sink_finalize (GObject * object);

static gboolean gst_soup_http_server_sink_start (GstBaseSink * sink);
static gboolean gst_soup_http_server_sink_stop (GstBaseSink * sink);
static GstFlowReturn gst_soup_http_server_sink_render (GstBaseSink * sink,
    GstBuffer * buffer);

static void gst_soup_http_server_new_message (SoupServer * server,
    SoupMessage * msg, const char *path, GHashTable * query,
    SoupClientContext * client, gpointer user_data);
static void gst_soup_http_server_message_finished (SoupMessage * msg,
    gpointer user_data);

enum
{
  PROP_0,
  PROP_PATH,
  PROP_PORT,
  PROP_LAST
};

/* pad templates */

static GstStaticPadTemplate gst_soup_http_server_sink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

/* class initialization */

#define gst_soup_http_server_sink_parent_class parent_class
G_DEFINE_TYPE (GstSoupHttpServerSink, gst_soup_http_server_sink,
    GST_TYPE_BASE_SINK);

static void
gst_soup_http_server_sink_class_init (GstSoupHttpServerSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_soup_http_client_sink_set_property;
  gobject_class->get_property = gst_soup_http_client_sink_get_property;
  gobject_class->finalize = gst_soup_http_server_sink_finalize;

  g_object_class_install_property (gobject_class,
      PROP_PORT,
      g_param_spec_uint ("port", "Port",
          "TCP port to serve on (0 = automatic)", 0, 65535,
          SOUP_ADDRESS_ANY_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_soup_http_server_sink_sink_template));

  gst_element_class_set_static_metadata (gstelement_class, "HTTP server sink",
      "Generic", "Publishes streams via HTTP",
      "Brendan Long <b.long@cablelabs.com>");

  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_soup_http_server_sink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_soup_http_server_sink_stop);
  base_sink_class->render =
      GST_DEBUG_FUNCPTR (gst_soup_http_server_sink_render);

  GST_DEBUG_CATEGORY_INIT (souphttpserversink_dbg, "souphttpserversink", 0,
      "souphttpserversink element");

}

static void
gst_soup_http_server_sink_init (GstSoupHttpServerSink * souphttpsink)
{
  g_mutex_init (&souphttpsink->mutex);

  souphttpsink->messages = g_hash_table_new (NULL, NULL);

  souphttpsink->path = 0;
  souphttpsink->port = SOUP_ADDRESS_ANY_PORT;
}


void
gst_soup_http_client_sink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (object);

  switch (property_id) {
    case PROP_PATH:
      if (souphttpsink->server) {
        GST_WARNING ("attempt to set path after server has started");
      } else {
        souphttpsink->path = g_value_get_string (value);
      }
      break;
    case PROP_PORT:
      if (souphttpsink->server) {
        GST_WARNING ("attempt to set port after server has started");
      } else {
        souphttpsink->port = g_value_get_uint (value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_soup_http_client_sink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (object);

  switch (property_id) {
    case PROP_PATH:
      g_value_set_string (value, souphttpsink->path);
      break;
    case PROP_PORT:
      g_value_set_uint (value, souphttpsink->port);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_soup_http_server_sink_finalize (GObject * object)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (object);

  g_hash_table_unref (souphttpsink->messages);
  g_mutex_clear (&souphttpsink->mutex);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_soup_http_server_sink_start (GstBaseSink * sink)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (sink);
  GST_DEBUG ("starting");

  g_mutex_lock (&souphttpsink->mutex);

  souphttpsink->server = soup_server_new ("port", souphttpsink->port, NULL);
  soup_server_add_handler (souphttpsink->server, souphttpsink->path,
      gst_soup_http_server_new_message, souphttpsink, NULL);
  soup_server_run_async (souphttpsink->server);

  g_mutex_unlock (&souphttpsink->mutex);

  GST_INFO ("new souphttpserversink starting on http://localhost:%u%s",
      soup_server_get_port (souphttpsink->server),
      souphttpsink->path ? souphttpsink->path : "");
  return TRUE;
}

static gboolean
gst_soup_http_server_sink_stop (GstBaseSink * sink)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (sink);
  GHashTableIter iter;
  gpointer key, value;
  GST_DEBUG ("stopping");

  g_mutex_lock (&souphttpsink->mutex);

  g_hash_table_iter_init (&iter, souphttpsink->messages);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    SoupMessage *msg = SOUP_MESSAGE (key);

    /* stop listening for finished signals, since the lock in that callback will
     * lead to deadlock */
    g_signal_handlers_disconnect_by_func (msg,
        G_CALLBACK (gst_soup_http_server_message_finished), souphttpsink);
  }
  g_hash_table_remove_all (souphttpsink->messages);

  g_object_unref (souphttpsink->server);

  g_mutex_unlock (&souphttpsink->mutex);

  GST_DEBUG ("stopped");
  return TRUE;
}

static GstFlowReturn
gst_soup_http_server_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (sink);
  GstMapInfo info = GST_MAP_INFO_INIT;
  GHashTableIter iter;
  gpointer key, value;

  if (!gst_buffer_map (buffer, &info, GST_MAP_READ)) {
    GST_ERROR ("Unable to map buffer");
    return GST_FLOW_ERROR;
  }

  if (info.size == 0) {
    GST_WARNING ("Buffer is empty");
    goto cleanup;
  }

  g_mutex_lock (&souphttpsink->mutex);
  g_hash_table_iter_init (&iter, souphttpsink->messages);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    SoupMessage *msg = SOUP_MESSAGE (key);
    soup_message_body_append (msg->response_body, SOUP_MEMORY_COPY, info.data,
        info.size);
    soup_server_unpause_message (souphttpsink->server, msg);
  }
  g_mutex_unlock (&souphttpsink->mutex);

cleanup:
  gst_buffer_unmap (buffer, &info);
  return GST_FLOW_OK;
}

static void
gst_soup_http_server_new_message (SoupServer * server, SoupMessage * msg,
    const char *path, GHashTable * query, SoupClientContext * client,
    gpointer user_data)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (user_data);
  GST_DEBUG ("new message");

  soup_message_headers_set_content_type (msg->response_headers, "text/plain",
      NULL);
  soup_message_headers_set_encoding (msg->response_headers,
      SOUP_ENCODING_CHUNKED);
  soup_message_set_status (msg, SOUP_STATUS_OK);

  g_mutex_lock (&souphttpsink->mutex);

  g_signal_connect (msg, "finished",
      G_CALLBACK (gst_soup_http_server_message_finished), souphttpsink);
  g_hash_table_add (souphttpsink->messages, msg);

  g_mutex_unlock (&souphttpsink->mutex);
}

static void
gst_soup_http_server_message_finished (SoupMessage * msg, gpointer user_data)
{
  GstSoupHttpServerSink *souphttpsink = GST_SOUP_HTTP_SERVER_SINK (user_data);
  GST_DEBUG ("message finished");

  g_mutex_lock (&souphttpsink->mutex);
  g_hash_table_remove (souphttpsink->messages, msg);
  g_mutex_unlock (&souphttpsink->mutex);

  GST_DEBUG ("finished removing message");
}
