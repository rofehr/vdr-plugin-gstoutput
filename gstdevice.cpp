#include "gstdevice.h"
#include <vdr/tools.h>
#include <string.h>

cGstDevice::cGstDevice(const char *VideoSink, const char *AudioSink, const char *Connector)
: videoSinkName(VideoSink), audioSinkName(AudioSink), connectorName(Connector)
{
}

cGstDevice::~cGstDevice()
{
  Shutdown();
}

bool cGstDevice::Init(void)
{
  //cMutexLock lock(&mutex);
  if (initialized)
    return true;

  if (!gst_is_initialized()) {
    int argc = 0;
    gst_init(&argc, nullptr);
  }

  if (!BuildVideoPipeline() || !BuildAudioPipeline()) {
    esyslog("gstoutput: pipeline construction failed");
    return false;
  }

  gst_element_set_state(video.pipeline, GST_STATE_READY);
  gst_element_set_state(audio.pipeline, GST_STATE_READY);

  initialized = true;
  return true;
}

// -----------------------------------------------------------------------
// Video pipeline:
//   appsrc(ts/es) -> tsdemux/h264parse -> decodebin -> videoconvert ->
//   compositor (sink_0 = decoded video, sink_1 = OSD overlay) -> videosink
//
// The compositor gives us OSD-over-video without a separate window
// manager, and works with kmssink/waylandsink for a DRM/KMS direct
// output path, matching the fbdev/Mali setup used elsewhere in this
// environment.
// -----------------------------------------------------------------------
bool cGstDevice::BuildVideoPipeline(void)
{
  video.pipeline = gst_pipeline_new("gst-video-pipeline");

  video.appsrc = gst_element_factory_make("appsrc", "video-src");
  g_object_set(video.appsrc,
               "is-live", TRUE,
               "format", GST_FORMAT_TIME,
               "do-timestamp", FALSE, // we set PTS ourselves from VDR's STC
               "block", TRUE,
               "max-bytes", (guint64)(4 * 1024 * 1024),
               nullptr);

  GstElement *parse    = gst_element_factory_make("h264parse", "video-parse");
  GstElement *decode    = gst_element_factory_make("decodebin", "video-decode");
  GstElement *convert   = gst_element_factory_make("videoconvert", "video-convert");
  compositor            = gst_element_factory_make("compositor", "video-mixer");
  GstElement *videosink = gst_element_factory_make(*videoSinkName, "video-sink");

  if (!video.appsrc || !parse || !decode || !convert || !compositor || !videosink) {
    esyslog("gstoutput: missing GStreamer element(s) for video pipeline "
            "(check that gstreamer1.0-plugins-{base,good,bad} and %s are installed)",
            *videoSinkName);
    return false;
  }

  const char *connStr = *connectorName;
  if (connStr && *connStr && !strcmp(*videoSinkName, "kmssink"))
    g_object_set(videosink, "connector-properties",
                 gst_structure_new("props", "connector-name", G_TYPE_STRING, connStr, nullptr),
                 nullptr);

  g_object_set(videosink, "sync", TRUE, nullptr);

  gst_bin_add_many(GST_BIN(video.pipeline), video.appsrc, parse, decode, convert,
                    compositor, videosink, nullptr);

  if (!gst_element_link(video.appsrc, parse)) {
    esyslog("gstoutput: failed to link appsrc -> h264parse");
    return false;
  }
  if (!gst_element_link(parse, decode)) {
    esyslog("gstoutput: failed to link h264parse -> decodebin");
    return false;
  }

  // decodebin exposes its source pad dynamically once it has determined
  // the stream's caps, so we hook "pad-added" to complete the link.
  g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer data) {
    GstElement *convert = GST_ELEMENT(data);
    GstPad *sinkpad = gst_element_get_static_pad(convert, "sink");
    if (!gst_pad_is_linked(sinkpad))
      gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
  }), convert);

  if (!gst_element_link(convert, compositor)) {
    esyslog("gstoutput: failed to link videoconvert -> compositor");
    return false;
  }
  if (!gst_element_link(compositor, videosink)) {
    esyslog("gstoutput: failed to link compositor -> videosink");
    return false;
  }

  // Request a second compositor sink pad for the OSD overlay branch;
  // cGstOsdProvider owns an appsrc that feeds into this pad.
  osdAppsrc = gst_element_factory_make("appsrc", "osd-src");
  g_object_set(osdAppsrc,
               "is-live", TRUE,
               "format", GST_FORMAT_TIME,
               "do-timestamp", FALSE,
               nullptr);
  GstElement *osdConvert = gst_element_factory_make("videoconvert", "osd-convert");
  gst_bin_add_many(GST_BIN(video.pipeline), osdAppsrc, osdConvert, nullptr);
  gst_element_link(osdAppsrc, osdConvert);

  GstPad *osdSrcPad  = gst_element_get_static_pad(osdConvert, "src");
  GstPad *mixerSink1 = gst_element_request_pad_simple(compositor, "sink_%u");
  gst_pad_link(osdSrcPad, mixerSink1);
  // OSD layer always on top, alpha-blended
  g_object_set(mixerSink1, "zorder", 1, nullptr);
  gst_object_unref(osdSrcPad);

  video.bus = gst_pipeline_get_bus(GST_PIPELINE(video.pipeline));
  video.watchId = gst_bus_add_watch(video.bus, BusCallback, this);

  return true;
}

bool cGstDevice::BuildAudioPipeline(void)
{
  audio.pipeline = gst_pipeline_new("gst-audio-pipeline");

  audio.appsrc = gst_element_factory_make("appsrc", "audio-src");
  g_object_set(audio.appsrc,
               "is-live", TRUE,
               "format", GST_FORMAT_TIME,
               "do-timestamp", FALSE,
               "block", TRUE,
               nullptr);

  GstElement *parse    = gst_element_factory_make("aacparse", "audio-parse"); // swap for ac3parse/mpegaudioparse as needed
  GstElement *decode   = gst_element_factory_make("decodebin", "audio-decode");
  GstElement *convert  = gst_element_factory_make("audioconvert", "audio-convert");
  GstElement *resample = gst_element_factory_make("audioresample", "audio-resample");
  GstElement *sink     = gst_element_factory_make(*audioSinkName, "audio-sink");

  if (!audio.appsrc || !parse || !decode || !convert || !resample || !sink) {
    esyslog("gstoutput: missing GStreamer element(s) for audio pipeline (sink=%s)", *audioSinkName);
    return false;
  }

  gst_bin_add_many(GST_BIN(audio.pipeline), audio.appsrc, parse, decode, convert, resample, sink, nullptr);

  gst_element_link(audio.appsrc, parse);
  gst_element_link(parse, decode);

  g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer data) {
    GstElement *convert = GST_ELEMENT(data);
    GstPad *sinkpad = gst_element_get_static_pad(convert, "sink");
    if (!gst_pad_is_linked(sinkpad))
      gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
  }), convert);

  gst_element_link_many(convert, resample, sink, nullptr);

  audio.bus = gst_pipeline_get_bus(GST_PIPELINE(audio.pipeline));
  audio.watchId = gst_bus_add_watch(audio.bus, BusCallback, this);

  return true;
}

gboolean cGstDevice::BusCallback(GstBus *, GstMessage *msg, gpointer data)
{
  cGstDevice *self = static_cast<cGstDevice *>(data);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = nullptr;
      gchar *dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      esyslog("gstoutput: GStreamer error: %s (%s)", err->message, dbg ? dbg : "no debug info");
      g_clear_error(&err);
      g_free(dbg);
      break;
    }
    case GST_MESSAGE_EOS:
      isyslog("gstoutput: end-of-stream on %s", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)));
      break;
    default:
      break;
  }
  (void)self;
  return TRUE;
}

void cGstDevice::Shutdown(void)
{
  //cMutexLock lock(&mutex);
  if (!initialized)
    return;

  if (video.pipeline) {
    gst_element_set_state(video.pipeline, GST_STATE_NULL);
    if (video.watchId) g_source_remove(video.watchId);
    if (video.bus) gst_object_unref(video.bus);
    gst_object_unref(video.pipeline);
    video.pipeline = nullptr;
  }
  if (audio.pipeline) {
    gst_element_set_state(audio.pipeline, GST_STATE_NULL);
    if (audio.watchId) g_source_remove(audio.watchId);
    if (audio.bus) gst_object_unref(audio.bus);
    gst_object_unref(audio.pipeline);
    audio.pipeline = nullptr;
  }
  initialized = false;
  playing = false;
}

bool cGstDevice::SetPlayMode(ePlayMode PlayMode)
{
  //cMutexLock lock(&mutex);
  switch (PlayMode) {
    case pmNone:
      gst_element_set_state(video.pipeline, GST_STATE_READY);
      gst_element_set_state(audio.pipeline, GST_STATE_READY);
      playing = false;
      break;
    case pmAudioVideo:
    case pmVideoOnly:
    case pmAudioOnly:
      gst_element_set_state(video.pipeline, GST_STATE_PLAYING);
      gst_element_set_state(audio.pipeline, GST_STATE_PLAYING);
      playing = true;
      break;
    default:
      break;
  }
  return true;
}

void cGstDevice::TrickSpeed(int Speed, bool Forward)
{
  // Seek-based trick speed: send a rate-only seek event downstream.
  // Real deployments generally combine this with I-frame-only delivery
  // from VDR (HasIBPTrickSpeed) to keep decodebin from starving.
  double rate = Forward ? (double)Speed : -(double)Speed;
  if (rate == 0) rate = 1.0;
  GstEvent *seek = gst_event_new_seek(rate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                                       GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE,
                                       GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  gst_element_send_event(video.pipeline, seek);
}

void cGstDevice::Clear(void)
{
  //cMutexLock lock(&mutex);
  if (video.appsrc) gst_element_send_event(video.appsrc, gst_event_new_flush_start());
  if (video.appsrc) gst_element_send_event(video.appsrc, gst_event_new_flush_stop(TRUE));
  if (audio.appsrc) gst_element_send_event(audio.appsrc, gst_event_new_flush_start());
  if (audio.appsrc) gst_element_send_event(audio.appsrc, gst_event_new_flush_stop(TRUE));
  cDevice::Clear();
}

void cGstDevice::Play(void)
{
  gst_element_set_state(video.pipeline, GST_STATE_PLAYING);
  gst_element_set_state(audio.pipeline, GST_STATE_PLAYING);
  cDevice::Play();
}

void cGstDevice::Freeze(void)
{
  gst_element_set_state(video.pipeline, GST_STATE_PAUSED);
  gst_element_set_state(audio.pipeline, GST_STATE_PAUSED);
  cDevice::Freeze();
}

void cGstDevice::Mute(void)
{
  if (audio.sink)
    g_object_set(audio.sink, "mute", TRUE, nullptr);
  cDevice::Mute();
}

void cGstDevice::StillPicture(const uchar *Data, int Length)
{
  // Push a single I-frame through the video appsrc and hold on PAUSED.
  PlayVideo(Data, Length);
  gst_element_set_state(video.pipeline, GST_STATE_PAUSED);
}

int cGstDevice::PlayVideo(const uchar *Data, int Length)
{
  if (!video.appsrc || Length <= 0)
    return Length;
  
  esyslog("gstoutput: PlayVideo Length %d", Length);
 
  GstBuffer *buf = gst_buffer_new_allocate(nullptr, Length, nullptr);
  gst_buffer_fill(buf, 0, Data, Length);

  // NOTE: VDR delivers PES/TS packets with PTS embedded in the packet
  // header; extract it here (omitted for brevity) and set:
  // GST_BUFFER_PTS(buf) = pts_in_ns;
  GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;

  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(video.appsrc), buf);
  if (ret != GST_FLOW_OK) {
    esyslog("gstoutput: video appsrc push failed (%d)", ret);
    return 0;
  }
  return Length;
}

int cGstDevice::PlayAudio(const uchar *Data, int Length, uchar Id)
{
  if (!audio.appsrc || Length <= 0)
    return Length;

  GstBuffer *buf = gst_buffer_new_allocate(nullptr, Length, nullptr);
  gst_buffer_fill(buf, 0, Data, Length);
  GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE; // see PlayVideo note

  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(audio.appsrc), buf);
  if (ret != GST_FLOW_OK) {
    esyslog("gstoutput: audio appsrc push failed (%d)", ret);
    return 0;
  }
  return Length;
}

int64_t cGstDevice::GetSTC(void)
{
  if (!video.pipeline)
    return -1;
  gint64 pos = 0;
  if (gst_element_query_position(video.pipeline, GST_FORMAT_TIME, &pos))
    return pos / 100; // convert ns -> 90kHz-ish scale used by VDR STC callers
  return -1;
}
