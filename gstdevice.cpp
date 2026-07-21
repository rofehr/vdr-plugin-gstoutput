#include "gstdevice.h"
#include <vdr/tools.h>
#include <string.h>

namespace {

// 33-bit PTS/DTS wrap boundary, per MPEG-2 Systems (ISO/IEC 13818-1).
constexpr int64_t PTS_MASK = 0x1FFFFFFFFLL;
constexpr int64_t PTS_HALF = PTS_MASK >> 1;

// Stream IDs that never carry a standard PES optional header (padding,
// program stream map/directory, private_stream_2, ECM/EMM, DSMCC, etc.).
bool StreamIdHasNoPesHeader(uchar StreamId)
{
  switch (StreamId) {
    case 0xBC: case 0xBE: case 0xBF: case 0xF0:
    case 0xF1: case 0xF2: case 0xFF:
      return true;
    default:
      return false;
  }
}

// Extracts the 33-bit, 90kHz PTS from a PES packet as delivered by VDR to
// cDevice::PlayVideo()/PlayAudio(). Returns false if Data isn't a
// start-code-prefixed PES packet, or carries no PTS (PTS_DTS_flags == 0).
bool ExtractPesPts(const uchar *Data, int Length, int64_t &Pts90k)
{
  if (Length < 14)
    return false;
  if (Data[0] != 0x00 || Data[1] != 0x00 || Data[2] != 0x01)
    return false;
  if (StreamIdHasNoPesHeader(Data[3]))
    return false;
  if ((Data[6] & 0xC0) != 0x80)
    return false;
  uchar ptsDtsFlags = (Data[7] >> 6) & 0x03;
  if (ptsDtsFlags == 0)
    return false;
  const uchar *p = Data + 9;
  if ((p[0] & 0xF0) != 0x20 && (p[0] & 0xF0) != 0x30)
    return false;

  int64_t b32_30 = (p[0] >> 1) & 0x07;
  int64_t b29_22 = p[1];
  int64_t b21_15 = (p[2] >> 1) & 0x7F;
  int64_t b14_7  = p[3];
  int64_t b6_0   = (p[4] >> 1) & 0x7F;

  Pts90k = (b32_30 << 30) | (b29_22 << 22) | (b21_15 << 15) | (b14_7 << 7) | b6_0;
  return true;
}

// Returns the byte offset in a PES packet where the actual elementary
// stream payload begins - i.e. past the PES header itself. Pushing the
// raw PES header into a codec parser corrupts NAL/frame boundary
// detection (observed as decode errors), so this must always be stripped.
int PesPayloadOffset(const uchar *Data, int Length)
{
  if (Length < 9 || Data[0] != 0x00 || Data[1] != 0x00 || Data[2] != 0x01)
    return 0;
  if ((Data[6] & 0xC0) != 0x80)
    return 6;
  int headerDataLen = Data[8];
  int offset = 9 + headerDataLen;
  if (offset > Length)
    return Length;
  return offset;
}

// Tries decoder elements in priority order (hardware first, software
// last) and returns the first one that actually instantiates on this
// system. Some hardware decoder families (the older "vaapidecode", as
// opposed to the newer unified "va" plugin's "vah264dec") need an
// explicit postprocessing element afterwards to get their output into a
// format videoconvert can handle - PostprocFactory carries that, or is
// left empty if not needed.
struct DecoderCandidate {
  const char *decoderFactory;
  const char *postprocFactory; // may be nullptr
};

bool TryCreateDecoder(const DecoderCandidate candidates[], int count,
                      GstElement **outDecoder, GstElement **outPostproc,
                      const char **outFactoryName)
{
  for (int i = 0; i < count; i++) {
    GstElement *dec = gst_element_factory_make(candidates[i].decoderFactory, "video-decoder");
    if (!dec)
      continue;
    GstElement *post = nullptr;
    if (candidates[i].postprocFactory) {
      post = gst_element_factory_make(candidates[i].postprocFactory, "video-postproc");
      if (!post) {
        gst_object_unref(dec);
        continue; // this candidate needs postproc but it's not available - try the next one
      }
    }
    *outDecoder = dec;
    *outPostproc = post;
    *outFactoryName = candidates[i].decoderFactory;
    return true;
  }
  return false;
}

} // namespace

// ===========================================================================
// cGstFeederThread
// ===========================================================================

cGstFeederThread::cGstFeederThread(GAsyncQueue *Queue, GstElement *AppSrc, const char *Name)
: cThread(Name), queue(Queue), appsrc(AppSrc)
{
  Start();
}

cGstFeederThread::~cGstFeederThread()
{
  Cancel(3); // ask Action()'s loop to stop and wait up to 3s for it to actually exit
}

void cGstFeederThread::Action(void)
{
  while (Running()) {
    // Timeout so we periodically re-check Running() for clean shutdown,
    // without busy-polling.
    GstBuffer *buf = static_cast<GstBuffer *>(g_async_queue_timeout_pop(queue, 100000 /* 100ms, in microseconds */));
    if (!buf)
      continue;
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf); // takes ownership of buf
    if (ret != GST_FLOW_OK)
      esyslog("gstoutput: feeder thread push failed on %s (%d)", GST_OBJECT_NAME(appsrc), ret);
  }
}

// ===========================================================================
// cGstDevice
// ===========================================================================

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
  cMutexLock lock(&mutex);
  if (initialized)
    return true;

  if (!gst_is_initialized()) {
    gst_debug_set_default_threshold(GST_LEVEL_INFO);
  
    int argc = 0;
    gst_init(&argc, nullptr);
  }

  if (!BuildVideoPipeline() || !BuildAudioPipeline()) {
    esyslog("gstoutput: pipeline construction failed");
    return false;
  }

  videoQueue = g_async_queue_new_full((GDestroyNotify)gst_buffer_unref);
  audioQueue = g_async_queue_new_full((GDestroyNotify)gst_buffer_unref);
  videoFeeder = new cGstFeederThread(videoQueue, video.appsrc, "gst-video-feeder");
  audioFeeder = new cGstFeederThread(audioQueue, audio.appsrc, "gst-audio-feeder");

  gst_element_set_state(video.pipeline, GST_STATE_READY);
  gst_element_set_state(audio.pipeline, GST_STATE_READY);

  PushInitialOsdFrame();

  initialized = true;
  return true;
}

// compositor (like any GStreamer mixer element) requires every sink pad to
// receive at least one buffer before the pipeline can complete preroll and
// actually start rendering. Our OSD appsrc only pushes a buffer when VDR
// calls cGstOsd::Flush() - i.e. when something is actually being shown on
// the OSD. During plain live TV viewing with no menu/OSD open, that pad
// would otherwise never preroll, and the *entire* pipeline (including the
// video branch that has plenty of frames) stays stuck rendering nothing.
void cGstDevice::PushInitialOsdFrame(void)
{
  if (!osdAppsrc)
    return;
  const int w = 1920, h = 1080; // matches the caps set on osdAppsrc
  GstBuffer *buf = gst_buffer_new_allocate(nullptr, (gsize)w * h * 4, nullptr);
  gst_buffer_memset(buf, 0, 0, gst_buffer_get_size(buf)); // fully transparent (alpha=0)
  GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
  PushOsdBuffer(buf); // also seeds the heartbeat cache, see PushOsdBuffer()/OsdHeartbeat()
}

// -----------------------------------------------------------------------
// Video pipeline (explicit chain, no decodebin):
//
//   appsrc -> h264parse(config-interval=-1) -> [decoder] -> [postproc?] ->
//   videoconvert -> queue -> compositor (sink_0) -> videosink
//
//   appsrc(OSD, BGRA) -> videoconvert -> capsfilter(BGRA) -> queue ->
//   compositor (sink_1, zorder=1, on top)
//
// [decoder] is chosen at runtime: VA-API first, then V4L2, then software
// as a last resort - see TryCreateDecoder(). No decodebin/dynamic pads
// anywhere in this graph, so the whole chain links statically up front.
// -----------------------------------------------------------------------
bool cGstDevice::BuildVideoPipeline(void)
{
  video.pipeline = gst_pipeline_new("gst-video-pipeline");

  video.appsrc = gst_element_factory_make("appsrc", "video-src");
  g_object_set(video.appsrc,
               "is-live", TRUE,
               "format", GST_FORMAT_TIME,
               // MUST be FALSE: this content commonly uses B-frames
               // (H.264 High Profile), decoded in a different order than
               // displayed. The decoder needs the real broadcast PTS to
               // reconstruct display order correctly - do-timestamp's
               // arrival-order stamping cannot provide that.
               "do-timestamp", FALSE,
               "block", TRUE,
               "max-bytes", (guint64)(256 * 1024),
               nullptr);

  GstElement *parse = gst_element_factory_make("h264parse", "video-parse");
  if (parse)
    g_object_set(parse, "config-interval", -1, nullptr); // (re-)send SPS/PPS before every IDR

  static const DecoderCandidate candidates[] = {
    { "vah264dec",      nullptr },  // new unified VA plugin (GStreamer >= 1.20ish), if present
    { "vaapidecodebin", nullptr },  // gstreamer-vaapi's self-contained decode+postproc bin -
                                     // some builds don't expose a standalone "vaapidecode" at all
    { "v4l2h264dec",    nullptr },  // V4L2 stateful/stateless M2M decoder
    { "avdec_h264",     nullptr },  // software fallback (ffmpeg-based)
  };
  GstElement *decoder = nullptr;
  GstElement *postproc = nullptr;
  const char *chosenFactory = nullptr;
  bool haveDecoder = TryCreateDecoder(candidates, (int)(sizeof(candidates) / sizeof(candidates[0])),
                                       &decoder, &postproc, &chosenFactory);
  if (haveDecoder)
    isyslog("gstoutput: video decoder: %s%s%s", chosenFactory,
            postproc ? " + " : "", postproc ? GST_OBJECT_NAME(postproc) : "");

  GstElement *convert     = gst_element_factory_make("videoconvert", "video-convert");
  GstElement *videoQueueEl = gst_element_factory_make("queue", "video-elastic-queue");
  compositor               = gst_element_factory_make("compositor", "video-mixer");
  GstElement *outputCapsFilter = gst_element_factory_make("capsfilter", "output-caps");
  GstElement *videosink   = gst_element_factory_make(*videoSinkName, "video-sink");

  if (outputCapsFilter) {
    // Force a single, fixed output size for the whole composited frame
    // (video + OSD combined), rather than letting compositor/kmssink
    // negotiate an arbitrary size - a mismatch between the negotiated
    // size and what the DRM plane actually expects showed up as
    // "drmModeSetPlane failed: Invalid argument" from kmssink.
    GstCaps *outCaps = gst_caps_new_simple("video/x-raw",
                                            "width",  G_TYPE_INT, 1920,
                                            "height", G_TYPE_INT, 1080,
                                            nullptr);
    g_object_set(outputCapsFilter, "caps", outCaps, nullptr);
    gst_caps_unref(outCaps);
  }

  struct { const char *name; GstElement *elem; } required[] = {
    { "appsrc",       video.appsrc },
    { "h264parse",    parse },
    { "decoder",      decoder },
    { "videoconvert", convert },
    { "queue",        videoQueueEl },
    { "compositor",   compositor },
    { "capsfilter",   outputCapsFilter },
    { *videoSinkName, videosink },
  };
  bool missing = false;
  for (auto &r : required) {
    if (!r.elem) {
      esyslog("gstoutput: GStreamer element factory '%s' not found - "
              "run 'gst-inspect-1.0 %s' on the target to confirm", r.name, r.name);
      missing = true;
    }
  }
  if (missing)
    return false;

  const char *connStr = *connectorName;
  if (connStr && *connStr && !strcmp(*videoSinkName, "kmssink"))
    g_object_set(videosink, "connector-properties",
                 gst_structure_new("props", "connector-name", G_TYPE_STRING, connStr, nullptr),
                 nullptr);

  g_object_set(videosink, "sync", TRUE, nullptr);
  // Disabled: a persistent gap between frame timestamps and the sink's
  // "earliest acceptable time" survived every attempt at fixing pipeline
  // latency negotiation/base_time, without any real CPU overload behind
  // it. Rather than keep chasing that, we stop vaapidecode from reacting
  // to QOS events at all, and stop the sink itself from refusing to
  // render buffers it considers late.
  g_object_set(videosink, "qos", FALSE, nullptr);
  g_object_set(videosink, "max-lateness", (gint64)-1, nullptr);

  if (videoQueueEl) {
    // Decoupling at the GStreamer scheduling level is now secondary (the
    // real decoupling is the GAsyncQueue feeder thread ahead of appsrc -
    // see cGstFeederThread), so this just needs to smooth minor jitter
    // between decode and compositor/sink, not protect VDR's own thread.
    g_object_set(videoQueueEl,
                 "max-size-time", (guint64)(100 * GST_MSECOND),
                 "max-size-bytes", (guint)0,
                 "max-size-buffers", (guint)0,
                 "leaky", 2, // GST_QUEUE_LEAK_DOWNSTREAM
                 nullptr);
  }

  gst_bin_add_many(GST_BIN(video.pipeline), video.appsrc, parse, decoder, convert,
                    videoQueueEl, compositor, outputCapsFilter, videosink, nullptr);
  if (postproc)
    gst_bin_add(GST_BIN(video.pipeline), postproc);

  bool linked = gst_element_link(video.appsrc, parse) &&
                (postproc ? (gst_element_link(parse, decoder) &&
                             gst_element_link(decoder, postproc) &&
                             gst_element_link(postproc, convert))
                          : (gst_element_link(parse, decoder) &&
                             gst_element_link(decoder, convert))) &&
                gst_element_link(convert, videoQueueEl) &&
                gst_element_link(compositor, outputCapsFilter) &&
                gst_element_link(outputCapsFilter, videosink);
  if (!linked) {
    esyslog("gstoutput: failed to link video pipeline chain");
    return false;
  }

  // Request the video input pad explicitly (rather than via the generic
  // gst_element_link convenience function) so we can size it to fill the
  // fixed 1920x1080 canvas - otherwise the compositor would place the
  // decoded content at its native 1280x720 in the top-left corner only.
  GstPad *videoQueueSrcPad = gst_element_get_static_pad(videoQueueEl, "src");
  GstPad *mixerSink0 = gst_element_request_pad_simple(compositor, "sink_%u");
  gst_pad_link(videoQueueSrcPad, mixerSink0);
  g_object_set(mixerSink0,
               "width",  1920,
               "height", 1080,
               "xpos",   0,
               "ypos",   0,
               "zorder", 0,
               nullptr);
  gst_object_unref(videoQueueSrcPad);

  // OSD overlay branch: a second appsrc feeding compositor's other input.
  osdAppsrc = gst_element_factory_make("appsrc", "osd-src");
  {
    GstCaps *osdCaps = gst_caps_new_simple("video/x-raw",
                                            "format",    G_TYPE_STRING, "BGRA",
                                            "width",     G_TYPE_INT,    1920,
                                            "height",    G_TYPE_INT,    1080,
                                            "framerate", GST_TYPE_FRACTION, 0, 1,
                                            nullptr);
    g_object_set(osdAppsrc,
                 // Genuinely needs to be TRUE: with is-live=FALSE,
                 // compositor's aggregator assumes a next buffer is
                 // always guaranteed to eventually arrive and blocks the
                 // *entire* output waiting for it once OSD goes quiet.
                 // Kept alive instead via a periodic heartbeat re-push -
                 // see PushOsdBuffer()/OsdHeartbeat().
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 // compositor's video-aggregator base class *requires*
                 // every buffer to carry a valid PTS ("Need timestamped
                 // buffers!" otherwise) - do-timestamp=TRUE has appsrc
                 // stamp each buffer with the current pipeline running
                 // time automatically.
                 "do-timestamp", TRUE,
                 "caps", osdCaps,
                 nullptr);
    gst_caps_unref(osdCaps);
  }
  GstElement *osdConvert     = gst_element_factory_make("videoconvert", "osd-convert");
  GstElement *osdCapsFilter  = gst_element_factory_make("capsfilter", "osd-capsfilter");
  GstElement *osdQueueEl     = gst_element_factory_make("queue", "osd-queue");
  {
    // Force the OSD branch to negotiate an alpha-capable format all the
    // way to the compositor pad, or videoconvert is free to silently drop
    // our transparency and blot out the whole picture at zorder=1.
    GstCaps *alphaCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", nullptr);
    g_object_set(osdCapsFilter, "caps", alphaCaps, nullptr);
    gst_caps_unref(alphaCaps);
  }
  if (osdQueueEl) {
    // Without ANY buffering element in this branch, it reports a
    // max-latency of 0 to compositor's aggregator latency negotiation,
    // which conflicts with the video branch's non-zero min-latency
    // requirement and breaks the whole pipeline, not just the overlay.
    g_object_set(osdQueueEl,
                 "max-size-time", (guint64)(200 * GST_MSECOND),
                 "max-size-bytes", (guint)0,
                 "max-size-buffers", (guint)0,
                 "leaky", 2,
                 nullptr);
  }
  gst_bin_add_many(GST_BIN(video.pipeline), osdAppsrc, osdConvert, osdCapsFilter, osdQueueEl, nullptr);
  gst_element_link_many(osdAppsrc, osdConvert, osdCapsFilter, osdQueueEl, nullptr);

  GstPad *osdSrcPad  = gst_element_get_static_pad(osdQueueEl, "src");
  GstPad *mixerSink1 = gst_element_request_pad_simple(compositor, "sink_%u");
  gst_pad_link(osdSrcPad, mixerSink1);
  g_object_set(mixerSink1,
               "width",  1920,
               "height", 1080,
               "xpos",   0,
               "ypos",   0,
               "zorder", 1, // OSD layer always on top
               nullptr);
  gst_object_unref(osdSrcPad);

  // Explicit black background rather than relying on this GStreamer
  // version's default (which is actually "checker", value 0).
  g_object_set(compositor, "background", 1 /* GST_COMPOSITOR_BACKGROUND_BLACK */, nullptr);

  video.bus = gst_pipeline_get_bus(GST_PIPELINE(video.pipeline));
  return true;
}

// -----------------------------------------------------------------------
// Audio pipeline: kept on decodebin (unlike video, the codec varies -
// AAC/AC3/MPEG audio - and decodebin's auto-plugging handles that
// variability without us hand-picking a parser per codec).
// -----------------------------------------------------------------------
bool cGstDevice::BuildAudioPipeline(void)
{
  audio.pipeline = gst_pipeline_new("gst-audio-pipeline");

  audio.appsrc = gst_element_factory_make("appsrc", "audio-src");
  g_object_set(audio.appsrc,
               "is-live", TRUE,
               "format", GST_FORMAT_TIME,
               "do-timestamp", FALSE, // real broadcast PTS - see video.appsrc above
               "block", TRUE,
               "max-bytes", (guint64)(256 * 1024),
               nullptr);

  GstElement *parse    = gst_element_factory_make("aacparse", "audio-parse"); // swap for ac3parse/mpegaudioparse as needed
  GstElement *decode   = gst_element_factory_make("decodebin", "audio-decode");
  GstElement *convert  = gst_element_factory_make("audioconvert", "audio-convert");
  GstElement *resample = gst_element_factory_make("audioresample", "audio-resample");
  GstElement *sink     = gst_element_factory_make(*audioSinkName, "audio-sink");
  GstElement *queue    = gst_element_factory_make("queue", "audio-elastic-queue");

  if (sink && !strcmp(*audioSinkName, "alsasink")) {
    g_object_set(sink,
                 "buffer-time", (gint64)1000000, // 1s
                 "latency-time", (gint64)40000,  // 40ms
                 nullptr);
  }
  if (queue) {
    g_object_set(queue,
                 "max-size-time", (guint64)(100 * GST_MSECOND),
                 "max-size-bytes", (guint)0,
                 "max-size-buffers", (guint)0,
                 "leaky", 2,
                 nullptr);
  }

  struct { const char *name; GstElement *elem; } required[] = {
    { "appsrc",        audio.appsrc },
    { "aacparse",      parse },
    { "decodebin",     decode },
    { "audioconvert",  convert },
    { "audioresample", resample },
    { "queue",         queue },
    { *audioSinkName,  sink },
  };
  bool missing = false;
  for (auto &r : required) {
    if (!r.elem) {
      esyslog("gstoutput: GStreamer element factory '%s' not found - "
              "run 'gst-inspect-1.0 %s' on the target to confirm", r.name, r.name);
      missing = true;
    }
  }
  if (missing)
    return false;

  gst_bin_add_many(GST_BIN(audio.pipeline), audio.appsrc, parse, decode, convert, resample, queue, sink, nullptr);

  gst_element_link(audio.appsrc, parse);
  gst_element_link(parse, decode);

  g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer data) {
    GstElement *convert = GST_ELEMENT(data);
    GstPad *sinkpad = gst_element_get_static_pad(convert, "sink");
    if (!gst_pad_is_linked(sinkpad))
      gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
  }), convert);

  gst_element_link_many(convert, resample, queue, sink, nullptr);
  audio.sink = sink;

  audio.bus = gst_pipeline_get_bus(GST_PIPELINE(audio.pipeline));
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
      esyslog("gstoutput: GStreamer error on %s: %s (%s)",
              GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err->message, dbg ? dbg : "no debug info");
      g_clear_error(&err);
      g_free(dbg);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *err = nullptr;
      gchar *dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      esyslog("gstoutput: GStreamer warning on %s: %s (%s)",
              GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err->message, dbg ? dbg : "no debug info");
      g_clear_error(&err);
      g_free(dbg);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->video.pipeline) ||
          GST_MESSAGE_SRC(msg) == GST_OBJECT(self->audio.pipeline)) {
        GstState oldState, newState, pending;
        gst_message_parse_state_changed(msg, &oldState, &newState, &pending);
        isyslog("gstoutput: %s state changed: %s -> %s (pending: %s)",
                GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
                gst_element_state_get_name(oldState),
                gst_element_state_get_name(newState),
                gst_element_state_get_name(pending));
      }
      break;
    }
    case GST_MESSAGE_EOS:
      isyslog("gstoutput: end-of-stream on %s", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)));
      break;
    default:
      break;
  }
  return TRUE;
}

void cGstDevice::PollBus(void)
{
  if (video.bus) {
    GstMessage *msg;
    while ((msg = gst_bus_pop(video.bus)) != nullptr) {
      BusCallback(video.bus, msg, this);
      gst_message_unref(msg);
    }
  }
  if (audio.bus) {
    GstMessage *msg;
    while ((msg = gst_bus_pop(audio.bus)) != nullptr) {
      BusCallback(audio.bus, msg, this);
      gst_message_unref(msg);
    }
  }

  OsdHeartbeat();
}

void cGstDevice::PushOsdBuffer(GstBuffer *Buffer)
{
  if (!osdAppsrc) {
    gst_buffer_unref(Buffer);
    return;
  }

  cMutexLock lock(&osdMutex);
  if (lastOsdBuffer)
    gst_buffer_unref(lastOsdBuffer);
  lastOsdBuffer = gst_buffer_ref(Buffer);
  osdHeartbeatTimer.Set(OSD_HEARTBEAT_MS);

  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(osdAppsrc), Buffer); // takes ownership
  if (ret != GST_FLOW_OK)
    esyslog("gstoutput: OSD appsrc push failed (%d)", ret);
}

void cGstDevice::OsdHeartbeat(void)
{
  if (!osdAppsrc)
    return;
  cMutexLock lock(&osdMutex);
  if (!lastOsdBuffer || !osdHeartbeatTimer.TimedOut())
    return;

  GstBuffer *copy = gst_buffer_copy(lastOsdBuffer);
  GST_BUFFER_PTS(copy) = GST_CLOCK_TIME_NONE; // re-timestamped fresh by do-timestamp=TRUE
  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(osdAppsrc), copy);
  if (ret != GST_FLOW_OK)
    esyslog("gstoutput: OSD heartbeat push failed (%d)", ret);
  osdHeartbeatTimer.Set(OSD_HEARTBEAT_MS);
}

void cGstDevice::Shutdown(void)
{
  cMutexLock lock(&mutex);
  if (!initialized)
    return;

  if (videoFeeder) {
    delete videoFeeder; // stops the thread (see ~cGstFeederThread)
    videoFeeder = nullptr;
  }
  if (audioFeeder) {
    delete audioFeeder;
    audioFeeder = nullptr;
  }
  if (videoQueue) {
    g_async_queue_unref(videoQueue);
    videoQueue = nullptr;
  }
  if (audioQueue) {
    g_async_queue_unref(audioQueue);
    audioQueue = nullptr;
  }

  if (video.pipeline) {
    gst_element_set_state(video.pipeline, GST_STATE_NULL);
    if (video.bus) gst_object_unref(video.bus);
    gst_object_unref(video.pipeline);
    video.pipeline = nullptr;
  }
  if (audio.pipeline) {
    gst_element_set_state(audio.pipeline, GST_STATE_NULL);
    if (audio.bus) gst_object_unref(audio.bus);
    gst_object_unref(audio.pipeline);
    audio.pipeline = nullptr;
  }
  initialized = false;
  playing = false;

  {
    cMutexLock osdLock(&osdMutex);
    if (lastOsdBuffer) {
      gst_buffer_unref(lastOsdBuffer);
      lastOsdBuffer = nullptr;
    }
  }

  if (sharedClock) {
    gst_object_unref(sharedClock);
    sharedClock = nullptr;
  }
}

void cGstDevice::SyncPipelineClocks(void)
{
  if (!sharedClock)
    sharedClock = gst_system_clock_obtain();

  // Just share the clock instance between both pipelines - manually
  // forcing base_time here (tried at length) had no measurable effect on
  // the sink's lateness judgments, so we don't fight that anymore; see
  // "qos"/"max-lateness" on kmssink instead.
  gst_pipeline_use_clock(GST_PIPELINE(video.pipeline), sharedClock);
  gst_pipeline_use_clock(GST_PIPELINE(audio.pipeline), sharedClock);
}

void cGstDevice::SuspendDisplay(void)
{
  if (video.pipeline)
    gst_element_set_state(video.pipeline, GST_STATE_NULL);
}

void cGstDevice::ResumeDisplay(void)
{
  if (video.pipeline)
    gst_element_set_state(video.pipeline, GST_STATE_READY);
}

bool cGstDevice::SetPlayMode(ePlayMode PlayMode)
{
  cMutexLock lock(&mutex);
  isyslog("gstoutput: SetPlayMode(%d) called", (int)PlayMode);
  switch (PlayMode) {
    case pmNone:
      gst_element_set_state(video.pipeline, GST_STATE_READY);
      gst_element_set_state(audio.pipeline, GST_STATE_READY);
      playing = false;
      break;
    case pmAudioVideo:
    case pmVideoOnly:
    case pmAudioOnly:
      SyncPipelineClocks();
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
  double rate = Forward ? (double)Speed : -(double)Speed;
  if (rate == 0) rate = 1.0;
  GstEvent *seek = gst_event_new_seek(rate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                                       GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE,
                                       GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  gst_element_send_event(video.pipeline, seek);
}

void cGstDevice::Clear(void)
{
  cMutexLock lock(&mutex);

  // Drop anything still queued for the feeder threads - stale data from
  // before a channel switch shouldn't play out afterwards.
  if (videoQueue) {
    GstBuffer *buf;
    while ((buf = static_cast<GstBuffer *>(g_async_queue_try_pop(videoQueue))) != nullptr)
      gst_buffer_unref(buf);
  }
  if (audioQueue) {
    GstBuffer *buf;
    while ((buf = static_cast<GstBuffer *>(g_async_queue_try_pop(audioQueue))) != nullptr)
      gst_buffer_unref(buf);
  }

  {
    cMutexLock ptsLock(&ptsMutex);
    ptsBaseline90k = -1;
    videoPtsState = cPtsUnwrap();
    audioPtsState = cPtsUnwrap();
    lastVideoPts = GST_CLOCK_TIME_NONE;
    lastAudioPts = GST_CLOCK_TIME_NONE;
  }

  tsToPesVideo.Reset();
  tsToPesAudio.Reset();
  ownPatPmtParser.Reset();

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
  PlayVideo(Data, Length);
  gst_element_set_state(video.pipeline, GST_STATE_PAUSED);
}

// Bounds the async queue manually (GAsyncQueue itself has no size limit):
// if GStreamer's side has fallen behind, drop the *oldest* queued buffer
// to make room, same "leaky downstream" behavior our GStreamer-side
// queues use, but applied before the data ever reaches GStreamer at all.
void cGstDevice::EnqueueBuffer(GAsyncQueue *Queue, GstBuffer *Buffer)
{
  if (!Queue) {
    gst_buffer_unref(Buffer);
    return;
  }
  while ((guint)g_async_queue_length(Queue) >= MAX_QUEUED_BUFFERS) {
    GstBuffer *old = static_cast<GstBuffer *>(g_async_queue_try_pop(Queue));
    if (!old)
      break;
    gst_buffer_unref(old);
  }
  g_async_queue_push(Queue, Buffer);
}

int cGstDevice::PlayVideo(const uchar *Data, int Length)
{
  static long callCount = 0;
  if (callCount == 0)
    isyslog("gstoutput: PlayVideo() called for the first time (Length=%d, first bytes=%02x %02x %02x %02x)",
            Length, Length > 0 ? Data[0] : 0, Length > 1 ? Data[1] : 0,
            Length > 2 ? Data[2] : 0, Length > 3 ? Data[3] : 0);
  if (++callCount % 200 == 0)
    isyslog("gstoutput: PlayVideo() called %ld times so far", callCount);

  if (!video.appsrc || Length <= 0)
    return Length;

  int64_t rawPts90k;
  bool havePts = ExtractPesPts(Data, Length, rawPts90k);

  int payloadOffset = PesPayloadOffset(Data, Length);
  const uchar *payload = Data + payloadOffset;
  int payloadLength = Length - payloadOffset;
  if (payloadLength <= 0)
    return Length;

  GstBuffer *buf = gst_buffer_new_allocate(nullptr, payloadLength, nullptr);
  gst_buffer_fill(buf, 0, payload, payloadLength);

  if (havePts) {
    GstClockTime pts = UnwrapAndOffsetPts(videoPtsState, rawPts90k);
    GST_BUFFER_PTS(buf) = pts;
    // Duration-from-PTS-delta: robust, self-adjusting, and doesn't need
    // to know the nominal framerate up front (we removed videorate,
    // which used to need that).
    if (GST_CLOCK_TIME_IS_VALID(lastVideoPts) && pts > lastVideoPts)
      GST_BUFFER_DURATION(buf) = pts - lastVideoPts;
    lastVideoPts = pts;
  }
  else {
    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE; // e.g. continuation packet without its own PES header
  }

  EnqueueBuffer(videoQueue, buf);
  return Length;
}

int cGstDevice::PlayAudio(const uchar *Data, int Length, uchar Id)
{
  static long callCount = 0;
  if (callCount == 0)
    isyslog("gstoutput: PlayAudio() called for the first time (Length=%d, Id=%d)", Length, Id);
  if (++callCount % 200 == 0)
    isyslog("gstoutput: PlayAudio() called %ld times so far", callCount);

  if (!audio.appsrc || Length <= 0)
    return Length;

  int64_t rawPts90k;
  bool havePts = ExtractPesPts(Data, Length, rawPts90k);

  int payloadOffset = PesPayloadOffset(Data, Length);
  const uchar *payload = Data + payloadOffset;
  int payloadLength = Length - payloadOffset;
  if (payloadLength <= 0)
    return Length;

  GstBuffer *buf = gst_buffer_new_allocate(nullptr, payloadLength, nullptr);
  gst_buffer_fill(buf, 0, payload, payloadLength);

  if (havePts) {
    GstClockTime pts = UnwrapAndOffsetPts(audioPtsState, rawPts90k);
    GST_BUFFER_PTS(buf) = pts;
    if (GST_CLOCK_TIME_IS_VALID(lastAudioPts) && pts > lastAudioPts)
      GST_BUFFER_DURATION(buf) = pts - lastAudioPts;
    lastAudioPts = pts;
  }
  else {
    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
  }

  EnqueueBuffer(audioQueue, buf);
  return Length;
}

// VDR's live TV and TS-based recording playback deliver raw 188-byte TS
// packets here (not ready-made PES packets), via cDvbPlayer/cTransfer ->
// cDevice::PlayTs(). We track PAT/PMT ourselves (ownPatPmtParser, since
// cDevice::patPmtParser is private in this VDR version) to learn the
// current video/audio PIDs, and reassemble each stream's PES packets
// with cTsToPes - mirroring the exact pattern VDR itself uses internally
// in cDevice::StillPicture(): on TsPayloadStart(), drain whatever PES
// data has accumulated so far, Reset(), *then* start accumulating the
// new packet.
int cGstDevice::PlayTs(const uchar *Data, int Length, bool VideoOnly)
{
  if (!Data || Length <= 0)
    return 0;

  static long tsCallCount = 0;
  if (tsCallCount == 0)
    isyslog("gstoutput: PlayTs() called for the first time (Length=%d, VideoOnly=%d)", Length, VideoOnly);
  if (++tsCallCount % 500 == 0)
    isyslog("gstoutput: PlayTs() called %ld times so far", tsCallCount);

  int played = 0;
  const uchar *p = Data;
  int remaining = Length;

  while (remaining >= TS_SIZE) {
    if (!TsError(p)) {
      int pid = TsPid(p);

      if (pid == PATPID)
        ownPatPmtParser.ParsePat(p, TS_SIZE);
      else if (ownPatPmtParser.IsPmtPid(pid))
        ownPatPmtParser.ParsePmt(p, TS_SIZE);
      else if (pid == ownPatPmtParser.Vpid()) {
        if (TsPayloadStart(p)) {
          int l;
          const uchar *pes;
          while ((pes = tsToPesVideo.GetPes(l)) != nullptr)
            PlayVideo(pes, l);
          tsToPesVideo.Reset();
        }
        tsToPesVideo.PutTs(p, TS_SIZE);
      }
      else if (!VideoOnly) {
        int apid = ownPatPmtParser.Apid(0);
        if (apid > 0 && pid == apid) {
          if (TsPayloadStart(p)) {
            int l;
            const uchar *pes;
            while ((pes = tsToPesAudio.GetPes(l)) != nullptr)
              PlayAudio(pes, l, pid);
            tsToPesAudio.Reset();
          }
          tsToPesAudio.PutTs(p, TS_SIZE);
        }
      }
    }

    played    += TS_SIZE;
    p         += TS_SIZE;
    remaining -= TS_SIZE;
  }
  return played;
}

GstClockTime cGstDevice::UnwrapAndOffsetPts(cPtsUnwrap &State, int64_t RawPts33)
{
  cMutexLock lock(&ptsMutex);

  if (State.last < 0) {
    State.extended = RawPts33;
  } else {
    int64_t diff = RawPts33 - State.last;
    if (diff < -PTS_HALF)
      diff += (PTS_MASK + 1);
    else if (diff > PTS_HALF)
      diff -= (PTS_MASK + 1);
    State.extended += diff;
  }
  State.last = RawPts33;

  if (ptsBaseline90k < 0)
    ptsBaseline90k = State.extended;

  int64_t rel90k = State.extended - ptsBaseline90k;
  if (rel90k < 0)
    rel90k = 0;

  return gst_util_uint64_scale(static_cast<guint64>(rel90k), GST_SECOND, 90000);
}

int64_t cGstDevice::GetSTC(void)
{
  if (!video.pipeline)
    return -1;
  gint64 posNs = 0;
  if (!gst_element_query_position(video.pipeline, GST_FORMAT_TIME, &posNs))
    return -1;

  int64_t pos90k = static_cast<int64_t>(gst_util_uint64_scale(static_cast<guint64>(posNs), 90000, GST_SECOND));

  cMutexLock lock(&ptsMutex);
  if (ptsBaseline90k < 0)
    return pos90k;

  return (pos90k + ptsBaseline90k) & PTS_MASK;
}
