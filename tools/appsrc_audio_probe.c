// Isolated test of the raw_local audio path. Faithful to the encoder: runtime
// caps, the exact downstream chain, mpegtsmux + filesink so the output can be
// probed, and pad probes counting what actually leaves each stage.
//
//   argv[1] layout  0 = non-interleaved, two memories (canonical planar)
//                   1 = non-interleaved, ONE memory (what the reader did)
//                   2 = interleaved, one memory
//   argv[2] caps when: "pre" = before PLAYING, "late" = after PLAYING
//   argv[3] output file
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 1024
#define CHANNELS 2
#define RATE 48000
#define NBUF 150

static GstElement *src;
static GstElement *pipeline;
static int variant;
static int pushed;

static GstPadProbeReturn count_probe(GstPad *pad,
                                     GstPadProbeInfo *info,
                                     gpointer user_data)
{
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    (*(int *) user_data)++;
  }
  return GST_PAD_PROBE_OK;
}

static GstCaps *make_caps(int variant)
{
  return gst_caps_new_simple("audio/x-raw",
                             "format",
                             G_TYPE_STRING,
                             "F32LE",
                             "layout",
                             G_TYPE_STRING,
                             (variant == 2) ? "interleaved" : "non-interleaved",
                             "rate",
                             G_TYPE_INT,
                             RATE,
                             "channels",
                             G_TYPE_INT,
                             CHANNELS,
                             NULL);
}

static gboolean push_one(gpointer unused)
{
  (void) unused;
  if (pushed >= NBUF) {
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    return G_SOURCE_REMOVE;
  }

  const gsize plane = FRAMES * 4;
  GstBuffer *buf = NULL;

  if (variant == 0) {
    buf = gst_buffer_new();
    for (int c = 0; c < CHANNELS; c++) {
      GstMemory *mem = gst_allocator_alloc(NULL, plane, NULL);
      GstMapInfo mi;
      gst_memory_map(mem, &mi, GST_MAP_WRITE);
      memset(mi.data, 0, mi.size);
      gst_memory_unmap(mem, &mi);
      gst_buffer_append_memory(buf, mem);
    }
  } else {
    const gsize total =
        (variant == 1) ? plane * CHANNELS : (gsize) FRAMES * CHANNELS * 4;
    buf = gst_buffer_new_allocate(NULL, total, NULL);
    GstMapInfo mi;
    gst_buffer_map(buf, &mi, GST_MAP_WRITE);
    memset(mi.data, 0, mi.size);
    gst_buffer_unmap(buf, &mi);
  }

  GST_BUFFER_PTS(buf) =
      gst_util_uint64_scale((guint64) pushed * FRAMES, GST_SECOND, RATE);
  GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(FRAMES, GST_SECOND, RATE);

  const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
  if (ret != GST_FLOW_OK) {
    g_print("push %d FAILED: %s\n", pushed, gst_flow_get_name(ret));
    return G_SOURCE_REMOVE;
  }
  pushed++;
  return G_SOURCE_CONTINUE;
}

static void report(GstMessage *msg)
{
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;
    gchar *dbg = NULL;
    gst_message_parse_error(msg, &err, &dbg);
    g_print("RESULT: ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src),
            err->message);
    g_error_free(err);
    g_free(dbg);
  } else {
    g_print("RESULT: EOS cleanly\n");
  }
}

int main(int argc, char **argv)
{
  variant = (argc > 1) ? atoi(argv[1]) : 0;
  const gboolean late = (argc > 2) && (strcmp(argv[2], "late") == 0);
  const char *out = (argc > 3) ? argv[3] : "/tmp/aptest.ts";
  gst_init(&argc, &argv);

  char desc[1024];
  snprintf(desc,
           sizeof(desc),
           "appsrc name=s is-live=true format=time do-timestamp=false "
           "max-bytes=1048576 ! queue silent=true ! audioresample ! "
           "audioconvert name=aconv ! avenc_aac ! aacparse ! queue silent=true "
           "! mpegtsmux name=mux ! filesink location=%s",
           out);
  pipeline = gst_parse_launch(desc, NULL);
  if (pipeline == NULL) {
    g_print("RESULT: pipeline failed to parse\n");
    return 1;
  }
  src = gst_bin_get_by_name(GST_BIN(pipeline), "s");

  static int out_appsrc = 0;
  static int out_aconv = 0;
  GstPad *pad = gst_element_get_static_pad(src, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe, &out_appsrc,
                    NULL);
  gst_object_unref(pad);
  GstElement *aconv = gst_bin_get_by_name(GST_BIN(pipeline), "aconv");
  pad = gst_element_get_static_pad(aconv, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe, &out_aconv,
                    NULL);
  gst_object_unref(pad);

  if (!late) {
    GstCaps *caps = make_caps(variant);
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);
  }

  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (late) {
    GstCaps *caps = make_caps(variant);
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);
  }

  // Drive the pushes from this thread and drain the bus as we go. The pipeline's
  // own streaming threads do the work, so no main loop is involved.
  GstBus *bus = gst_element_get_bus(pipeline);
  gboolean finished = FALSE;
  for (int i = 0; i < NBUF && !finished; i++) {
    const gboolean more = push_one(NULL);
    GstMessage *m =
        gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (m != NULL) {
      report(m);
      gst_message_unref(m);
      finished = TRUE;
      break;
    }
    if (!more) {
      break;
    }
    g_usleep(5000);
  }
  if (!finished) {
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    for (int i = 0; i < 40 && !finished; i++) {
      GstMessage *m = gst_bus_timed_pop_filtered(
          bus, 100 * GST_MSECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
      if (m != NULL) {
        report(m);
        gst_message_unref(m);
        finished = TRUE;
      }
    }
    if (!finished) {
      g_print("RESULT: no error and no EOS after the stream ended\n");
    }
  }
  g_print("buffers out of appsrc: %d, out of audioconvert: %d, pushed: %d\n",
          out_appsrc,
          out_aconv,
          pushed);

  gst_element_set_state(pipeline, GST_STATE_NULL);
  return 0;
}
