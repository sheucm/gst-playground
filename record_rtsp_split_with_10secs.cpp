#include <gst/gst.h>
#include <string>
#include <iostream>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>

// Craete a pipeline...
// bin = ghost_pad ! mp4mux ! filesink
// pipeline = rtspsrc ! rtph264depay ! h264parse ! queue ! bin

/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData {
  GstElement *pipeline;  /* Our one and only element */
  GstElement *bin;
  GstElement *rtspsrc;
  GstElement *rtph264depay;
  GstElement *h264parse;
  GstElement *queue;
  gboolean playing;      /* Are we in the PLAYING state? */
  gboolean terminate;    /* Should we terminate execution? */
  gint64 cuttime;
  std::string filesink_name;
} CustomData;


static void handle_message (CustomData *data, GstMessage *msg);
static void pad_added_handler (GstElement *src, GstPad *pad, CustomData *data);
static void change_filesink (CustomData *data);
static GstPadProbeReturn blocked_handler(GstPad *queue_srcpad, GstPadProbeInfo *info, CustomData *data);
static GstPadProbeReturn eos_handler(GstPad *bin_srcpad, GstPadProbeInfo *info, gpointer user_data);
static GstElement *new_custom_bin (CustomData *data);


int main(int argc, char *argv[]) {
  CustomData data;
  GstBus *bus;
  GstMessage *msg;
  GstStateChangeReturn ret;
  GstPad *pad;

  data.playing = FALSE;
  data.terminate = FALSE;
  data.cuttime = 10;

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Create the elements */
  data.rtspsrc = gst_element_factory_make("rtspsrc", NULL);
  data.rtph264depay = gst_element_factory_make("rtph264depay", NULL);
  data.h264parse = gst_element_factory_make("h264parse", NULL);
  data.queue = gst_element_factory_make("queue", NULL);

  data.pipeline = gst_pipeline_new("pipeline");
  data.bin = new_custom_bin(&data);

  if (!data.pipeline || !data.bin 
    || !data.rtspsrc || !data.rtph264depay 
    || !data.h264parse || !data.queue) {
    g_printerr ("Not all elements could be created.\n");
    return -1;
  }

  /* Build the pipeline. Note that we are NOT linking the source at this
   * point. We will do it later. */
  gst_bin_add_many (GST_BIN (data.pipeline), data.rtspsrc, 
    data.rtph264depay, data.h264parse, data.queue, data.bin, NULL);
  

  // Link elements
  if (!gst_element_link_many (
    data.rtph264depay, data.h264parse, data.queue, data.bin, NULL)) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }


  // Set elements' properties
  g_object_set (data.rtspsrc, 
    "location", "rtsp://admin:pass@192.168.85.7/rtsph2641080p", 
    "protocols", 0x00000004, // tcp
    NULL);


  // Connect to pad-added signal
  g_signal_connect (data.rtspsrc, "pad-added", G_CALLBACK(pad_added_handler), &data); 


  // State Playing
  ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
  if(ret == GST_STATE_CHANGE_FAILURE){
    g_printerr("Unable to set the pipeline to the playing state!\n");
    gst_object_unref(data.pipeline);
    return -1;
  }

  // Listen to bus
  bus = gst_element_get_bus(data.pipeline);
  do {
    msg = gst_bus_timed_pop_filtered (bus, 100 * GST_MSECOND,
        (GstMessageType)(GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR 
          | GST_MESSAGE_EOS));

    /* Parse message */
    if (msg != NULL) {
      handle_message(&data, msg);
    } else {

      /* We got no message, this means the timeout expired */
      if (data.playing) {
        gint64 current = -1;

        /* Query the current position of the stream */
        if (!gst_element_query_position (data.pipeline, GST_FORMAT_TIME, &current)) {
          g_printerr ("Could not query current position.\n");
        }

        /* Print current position and total duration */
        g_print ("Position %" GST_TIME_FORMAT "\r",
            GST_TIME_ARGS (current));

        // Simulate 10s per video duration
        if (current > data.cuttime * GST_SECOND) {
          if (data.cuttime >= 30){
            g_print("Reached over or equal 30 seconds, send eos to pipeline ...\n");
            gst_element_send_event(data.pipeline, gst_event_new_eos());
          } else {
            g_print("Performing change_filesink process...\n");
            change_filesink(&data);
            data.cuttime += 10;  
          }
        }
      }
    }
  } while (!data.terminate);

  g_print("Free resources\n");

  // Free Resources
  gst_object_unref(bus);
  gst_element_set_state(data.pipeline, GST_STATE_NULL);
  gst_object_unref(data.pipeline);  

  return 0;
}

static void handle_message (CustomData *data, GstMessage *msg) {
  
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err;
      gchar *debug_info;
      gst_message_parse_error (msg, &err, &debug_info);
      g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
      g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
      g_clear_error (&err);
      g_free (debug_info);
      data->terminate = TRUE;
    } break;
    case GST_MESSAGE_EOS:
      g_print ("End-Of-Stream reached.\n");
      data->terminate = TRUE;
      break;
    case GST_MESSAGE_STATE_CHANGED: {
      GstState old_state, new_state, pending_state;
      gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        g_print ("Pipeline state changed from %s to %s:\n",
            gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));

        /* Remember whether we are in the PLAYING state or not */
        data->playing = (new_state == GST_STATE_PLAYING);
      }
    } break;
    default:
      /* We should not reach here */
      g_printerr ("Unexpected message received.\n");
      break;
  }
  gst_message_unref (msg);
}

static void pad_added_handler(GstElement *src, GstPad *new_pad, CustomData * data)
{
  GstPad *sink_pad = gst_element_get_static_pad(data->rtph264depay, "sink");
  GstPadLinkReturn ret;
  GstCaps *new_pad_caps = NULL;
  GstStructure *new_pad_struct = NULL;
  const gchar *new_pad_type = NULL;

  g_print("Receiced new pad '%s' from '%s'.\n", 
    GST_PAD_NAME(new_pad), GST_ELEMENT_NAME(src));

  /* If our converter is already linked, we have nothing to do here */
  if (gst_pad_is_linked (sink_pad)) {
    g_print ("We are already linked. Ignoring.\n");
    goto exit;
  }

  /* Check the new pad's type */
  new_pad_caps = gst_pad_get_current_caps (new_pad);
  new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
  new_pad_type = gst_structure_get_name (new_pad_struct);
  if (!g_str_has_prefix(new_pad_type, "application/x-rtp")) {
    g_print("It has type '%s' which is not application/x-rtp. Ignoring.\n", new_pad_type);
    goto exit;
  }

  // Attempt the link
  ret = gst_pad_link (new_pad, sink_pad);
  if (GST_PAD_LINK_FAILED (ret)) {
    g_print ("Type is '%s' but link failed.\n", new_pad_type);
  } else {
    g_print ("Link succeeded (type '%s').\n", new_pad_type);
  }

exit:
  /* Unreference the new pad's caps, if we got them */
  if (new_pad_caps != NULL)
    gst_caps_unref (new_pad_caps);

  /* Unreference the sink pad */
  gst_object_unref (sink_pad);
}

static GstElement *new_custom_bin (CustomData *data)
{
  GstElement *bin;
  GstElement *mp4mux;
  GstElement *filesink;
  GstPad *pad;

  // create uuid
  boost::uuids::uuid uuid = boost::uuids::random_generator()();
  std::string uuid_str = boost::lexical_cast<std::string>(uuid);

  mp4mux = gst_element_factory_make("mp4mux", NULL);
  filesink = gst_element_factory_make("filesink", uuid_str.c_str());
  bin = gst_bin_new(NULL);

  if (!bin || !mp4mux || !filesink) {
    g_printerr ("Not all elements in bin could be created.\n");
    return NULL;
  }

  gst_bin_add_many(GST_BIN(bin), mp4mux, filesink, NULL);

  
  // Add ghost pad
  pad = gst_element_get_request_pad(mp4mux, "video_0");
  gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
  gst_object_unref(pad);


  if (!gst_element_link (mp4mux, filesink)) {
    g_printerr ("Elements of bin could not be linked.\n");
    gst_object_unref (bin);
    return NULL;
  }

  // create uuid and location
  std::string video = "/home/eric/Desktop/" + uuid_str + ".mp4";

  // set filesink location
  g_object_set (filesink, "location", video.c_str(), NULL);

  // set data->filesink_name
  data->filesink_name = uuid_str;
  
  return bin;
}


static void change_filesink (CustomData *data) 
{
  GstPad *queue_srcpad = gst_element_get_static_pad(data->queue, "src");

  g_print("Add blocked-probe on queue-srcpad...\n");

  // Block the queue_srcpad
  gst_pad_add_probe(queue_srcpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, 
                (GstPadProbeCallback)blocked_handler, data, NULL);
  
  gst_object_unref(queue_srcpad);
}

static GstPadProbeReturn blocked_handler(GstPad *queue_srcpad, GstPadProbeInfo *info, CustomData *data)
{
  GstPad *bin_sinkpad;
  GstElement *filesink;
  GstPad *filesink_pad;

  g_print("The queue-srcpad is blocked now !\n");

  // remove the probe first 
  gst_pad_remove_probe (queue_srcpad, GST_PAD_PROBE_INFO_ID (info));



  g_print("Install eos-probe on filesink-sinkpad...\n");
  
  filesink = gst_bin_get_by_name (GST_BIN (data->pipeline), data->filesink_name.c_str());

  filesink_pad = gst_element_get_static_pad(filesink, "sink");

  gst_pad_add_probe(filesink_pad, 
    (GstPadProbeType)(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), 
    eos_handler, data, NULL);

  gst_object_unref(filesink_pad);
  gst_object_unref(filesink);



  g_print("Send eos event to bin-sinkpad...\n");

  bin_sinkpad = gst_element_get_static_pad(data->bin, "sink");
  
  gst_pad_send_event(bin_sinkpad, gst_event_new_eos());
  
  gst_object_unref(bin_sinkpad);



  return GST_PAD_PROBE_OK;
}


static GstPadProbeReturn eos_handler(GstPad *bin_srcpad, GstPadProbeInfo *info, gpointer user_data)
{
  CustomData *data = (CustomData *)user_data;
  GstElement *newbin;

  if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info)) != GST_EVENT_EOS) 
    return GST_PAD_PROBE_PASS;

  // Remove the probe first
  gst_pad_remove_probe(bin_srcpad, GST_PAD_PROBE_INFO_ID(info));


  // craete new bin
  newbin = new_custom_bin(data);
  if (!newbin) {
    g_printerr("Custom bin cannot be created!\n");
    return GST_PAD_PROBE_DROP;
  }  

  g_print("Bin switching process start:\n");


  gst_element_set_state(data->bin, GST_STATE_NULL);

  // remove unlinks automatically
  g_print("removing current bin...\n");
  gst_bin_remove(GST_BIN(data->pipeline), data->bin);

  g_print("adding new bin...\n");
  gst_bin_add(GST_BIN(data->pipeline), newbin);

  g_print("linking...\n");
  gst_element_link(data->queue, newbin);

  // sync with pipeline state
  gst_element_sync_state_with_parent(newbin);

  // update data->bin
  data->bin = newbin;

  g_print("done!\n");

  // Drop it; otherwise pipeline will receive this eos event.
  return GST_PAD_PROBE_DROP;
}