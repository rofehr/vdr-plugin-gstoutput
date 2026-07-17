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
//
// PES header layout (relevant part), ISO/IEC 13818-1 2.4.3.7:
//   [0..2]  packet_start_code_prefix = 00 00 01
//   [3]     stream_id
//   [4..5]  PES_packet_length
//   [6]     '10' + scrambling + priority + alignment + copyright + original
//   [7]     PTS_DTS_flags(2) + ESCR_flag + ES_rate_flag + ... (1 byte)
//   [8]     PES_header_data_length
//   [9..13] PTS (5 bytes, present iff PTS_DTS_flags != 0)
bool ExtractPesPts(const uchar *Data, int Length, int64_t &Pts90k)
{
  if (Length < 14)
    return false;
  if (Data[0] != 0x00 || Data[1] != 0x00 || Data[2] != 0x01)
    return false; // not PES-framed (e.g. a bare ES fragment) - caller falls back to no-PTS
  if (StreamIdHasNoPesHeader(Data[3]))
    return false;
  if ((Data[6] & 0xC0) != 0x80)
    return false; // not an MPEG-2 optional PES header
  uchar ptsDtsFlags = (Data[7] >> 6) & 0x03;
  if (ptsDtsFlags == 0)
    return false; // no PTS in this packet
  const uchar *p = Data + 9;
  if ((p[0] & 0xF0) != 0x20 && (p[0] & 0xF0) != 0x30)
    return false; // marker nibble mismatch, malformed/unexpected header

  int64_t b32_30 = (p[0] >> 1) & 0x07;
  int64_t b29_22 = p[1];
  int64_t b21_15 = (p[2] >> 1) & 0x7F;
  int64_t b14_7  = p[3];
  int64_t b6_0   = (p[4] >> 1) & 0x7F;

  Pts90k = (b32_30 << 30) | (b29_22 << 22) | (b21_15 << 15) | (b14_7 << 7) | b6_0;
  return true;
}

// Returns the byte offset in a PES packet where the actual elementary
// stream payload begins - i.e. past the PES header itself. We were
// previously pushing the *entire* PES packet (header included) into
// appsrc/h264parse, which occasionally corrupts NAL unit boundary
// detection (observed as "unknown NAL unit type id 0, skip" from the
// VA-API H.264 decoder, eventually escalating into a fatal streaming
// error). Falls back to 6 (bare fixed header, no optional fields) or 0
// (not PES-framed at all - e.g. a raw continuation fragment, pass through
// unmodified) if this doesn't look like a standard PES optional header.
int PesPayloadOffset(const uchar *Data, int Length)
{
  if (Length < 9 || Data[0] != 0x00 || Data[1] != 0x00 || Data[2] != 0x01)
    return 0;
  if ((Data[6] & 0xC0) != 0x80)
    return 6;
  int headerDataLen = Data[8];
  int offset = 9 + headerDataLen;
  if (offset > Length)
    return Length; // malformed/truncated - avoid a negative payload length
  return offset;
}

} // namespace

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
    int argc = 0;
    gst_init(&argc, nullptr);
    //gst_debug_set_default_threshold(GST_LEVEL_ERROR);
	gst_debug_set_threshold_for_name("gst_base_sink", GST_LEVEL_LOG);
	gst_debug_set_threshold_for_name("gst_clock", GST_LEVEL_LOG);
	gst_debug_set_threshold_for_name("gst_qos", GST_LEVEL_LOG);
	gst_debug_set_threshold_for_name("v4l2*", GST_LEVEL_DEBUG);
	gst_debug_set_threshold_for_name("*kmssink*", GST_LEVEL_LOG);

  }

  if (!BuildVideoPipeline() || !BuildAudioPipeline()) {
    esyslog("gstoutput: pipeline construction failed");
    return false;
  }

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
  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(osdAppsrc), buf);
  if (ret != GST_FLOW_OK)
    esyslog("gstoutput: initial OSD preroll frame push failed (%d)", ret);
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
			   "sync", FALSE, 
               "max-bytes", (guint64)(4 * 1024 * 1024),
               nullptr);

  GstElement *parse    = gst_element_factory_make("h264parse", "video-parse");
  GstElement *decode    = gst_element_factory_make("decodebin", "video-decode");
  GstElement *convert   = gst_element_factory_make("videoconvert", "video-convert");
  GstElement *videoQueue = gst_element_factory_make("queue", "video-elastic-queue");
  compositor            = gst_element_factory_make("compositor", "video-mixer");
  GstElement *videosink = gst_element_factory_make(*videoSinkName, "video-sink");

  struct { const char *name; GstElement *elem; } required[] = {
    { "appsrc",           video.appsrc },
    { "h264parse",        parse },
    { "decodebin",        decode },
    { "videoconvert",     convert },
    { "queue",            videoQueue },
    { "compositor",       compositor },
    { *videoSinkName,     videosink },
  };
  bool missing = false;
  for (auto &r : required) {
    if (!r.elem) {
      esyslog("gstoutput: GStreamer element factory '%s' not found — "
              "run 'gst-inspect-1.0 %s' on the target to confirm, then check "
              "which gstreamer1.0-plugins-{base,good,bad} package provides it "
              "and whether it's in your image/recipe DEPENDS+RDEPENDS", r.name, r.name);
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
  // Marginal (single-digit-ms) lateness was triggering vaapidecode's QoS
  // frame-dropping via QOS events sent upstream from this sink, which in
  // turn seems to destabilize vaapidecodebin's internal queue - observed
  // as "Dropping frame due to QoS" immediately followed by a fatal
  // streaming error / spontaneous EOS. Disabling QoS reporting trades
  // graceful frame-dropping under sustained real overload for not
  // triggering this crash on brief, likely-harmless timing jitter.
  g_object_set(videosink, "qos", FALSE, nullptr);

  if (videoQueue) {
    // Same rationale as the audio queue: decouples the sink's real-time
    // sync-blocking render loop into its own GStreamer streaming thread,
    // so gst_app_src_push_buffer() (called synchronously from VDR's own
    // receiver thread via PlayTs()/PlayVideo()) never blocks waiting for
    // kmssink's display timing. Without this, VDR's own DVB receive ring
    // buffer overflows because it can't be drained fast enough.
    g_object_set(videoQueue,
                 "max-size-time", (guint64)(500 * GST_MSECOND),
                 "max-size-bytes", (guint)0,
                 "max-size-buffers", (guint)0,
                 "leaky", 2, // GST_QUEUE_LEAK_DOWNSTREAM: drop oldest frames on sustained overrun
                 nullptr);
  }

  gst_bin_add_many(GST_BIN(video.pipeline), video.appsrc, parse, decode, convert,
                    videoQueue, compositor, videosink, nullptr);

  // Log every element decodebin auto-plugs internally (parsers, decoders,
  // etc.) so we can confirm at runtime whether it picked a VA-API hardware
  // decoder (element/factory names starting with "va"/"vaapi") or fell
  // back to a software decoder (e.g. avdec_h264) - the latter is a common
  // cause of the host CPU maxing out a core during playback.
  g_signal_connect(decode, "deep-element-added", G_CALLBACK(+[](GstBin *, GstBin *, GstElement *element, gpointer) {
    GstElementFactory *factory = gst_element_get_factory(element);
    const char *factoryName = factory ? GST_OBJECT_NAME(factory) : "(unknown)";
    isyslog("gstoutput: video decodebin auto-plugged: %s", factoryName);
  }), nullptr);

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
    GstCaps *caps = gst_pad_get_current_caps(pad);
    gchar *capsStr = caps ? gst_caps_to_string(caps) : g_strdup("(no caps yet)");
    isyslog("gstoutput: video decodebin pad-added, caps=%s", capsStr);
    g_free(capsStr);
    if (caps) gst_caps_unref(caps);
    GstPad *sinkpad = gst_element_get_static_pad(convert, "sink");
    if (!gst_pad_is_linked(sinkpad))
      gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
  }), convert);

  if (!gst_element_link(convert, videoQueue)) {
    esyslog("gstoutput: failed to link videoconvert -> queue");
    return false;
  }
  if (!gst_element_link(videoQueue, compositor)) {
    esyslog("gstoutput: failed to link queue -> compositor");
    return false;
  }
  if (!gst_element_link(compositor, videosink)) {
    esyslog("gstoutput: failed to link compositor -> videosink");
    return false;
  }

  // Request a second compositor sink pad for the OSD overlay branch;
  // cGstOsdProvider owns an appsrc that feeds into this pad.
  osdAppsrc = gst_element_factory_make("appsrc", "osd-src");
  {
    // Raw video has no self-describing byte stream (unlike H.264), so
    // without explicit caps here GStreamer has no way to know the
    // width/height/format of what we're pushing - the result is exactly
    // the "garbage / checkerboard" look, not a clean OSD overlay.
    // NOTE: 1920x1080 matches the hardcoded value in cGstOsd (gstosd.cpp);
    // both need to move together to a real queried resolution eventually
    // (see README TODOs).
    GstCaps *osdCaps = gst_caps_new_simple("video/x-raw",
                                            "format",    G_TYPE_STRING, "BGRA",
                                            "width",     G_TYPE_INT,    1920,
                                            "height",    G_TYPE_INT,    1080,
                                            "framerate", GST_TYPE_FRACTION, 0, 1,
                                            nullptr);
    g_object_set(osdAppsrc,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", TRUE,
                 "caps", osdCaps,
                 nullptr);
    gst_caps_unref(osdCaps);
  }
  GstElement *osdConvert    = gst_element_factory_make("videoconvert", "osd-convert");
  GstElement *osdCapsFilter = gst_element_factory_make("capsfilter", "osd-capsfilter");
  GstElement *osdQueue      = gst_element_factory_make("queue", "osd-queue");
  {
    // Force the OSD branch to negotiate an alpha-capable format all the
    // way to the compositor pad. Without this, videoconvert is free to
    // pick any format compositor's sink pad accepts - including a
    // non-alpha one - which silently drops our transparency and turns
    // the (meant-to-be-transparent) OSD buffer into an opaque layer that
    // blacks out the whole picture at zorder=1.
    GstCaps *alphaCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", nullptr);
    g_object_set(osdCapsFilter, "caps", alphaCaps, nullptr);
    gst_caps_unref(alphaCaps);
  }
  if (osdQueue) {
    // Without ANY buffering element in this branch, it reports a
    // max-latency of 0 to compositor's aggregator latency negotiation,
    // which conflicts with the video branch's non-zero min-latency
    // requirement ("Impossible to configure latency: max 0 < min ...")
    // and breaks the whole pipeline - not just the OSD overlay. A small
    // queue is enough; OSD updates are infrequent and don't need the
    // half-second of headroom the video/audio elastic queues carry.
    g_object_set(osdQueue,
                 "max-size-time", (guint64)(200 * GST_MSECOND),
                 "max-size-bytes", (guint)0,
                 "max-size-buffers", (guint)0,
                 "leaky", 2, // GST_QUEUE_LEAK_DOWNSTREAM
                 nullptr);
  }
  gst_bin_add_many(GST_BIN(video.pipeline), osdAppsrc, osdConvert, osdCapsFilter, osdQueue, nullptr);
  gst_element_link_many(osdAppsrc, osdConvert, osdCapsFilter, osdQueue, nullptr);

  GstPad *osdSrcPad  = gst_element_get_static_pad(osdQueue, "src");
  GstPad *mixerSink1 = gst_element_request_pad_simple(compositor, "sink_%u");
  gst_pad_link(osdSrcPad, mixerSink1);
  // OSD layer always on top, alpha-blended
  g_object_set(mixerSink1, "zorder", 1, nullptr);
  gst_object_unref(osdSrcPad);

  // Explicit black background rather than relying on this GStreamer
  // version's default - rules out compositor's own placeholder pattern
  // as a source of visual artifacts entirely.
  g_object_set(compositor, "background", 1 /* GST_COMPOSITOR_BACKGROUND_BLACK */, nullptr);

  video.bus = gst_pipeline_get_bus(GST_PIPELINE(video.pipeline));


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
               "max-bytes", (guint64)(256 * 1024), // throttle VDR feed to sink's real drain rate
               nullptr);

  GstElement *parse    = gst_element_factory_make("aacparse", "audio-parse"); // swap for ac3parse/mpegaudioparse as needed
  GstElement *decode   = gst_element_factory_make("decodebin", "audio-decode");
  GstElement *convert  = gst_element_factory_make("audioconvert", "audio-convert");
  GstElement *resample = gst_element_factory_make("audioresample", "audio-resample");
  GstElement *sink     = gst_element_factory_make(*audioSinkName, "audio-sink");
  GstElement *queue    = gst_element_factory_make("queue", "audio-elastic-queue");

  if (sink && !strcmp(*audioSinkName, "alsasink")) {
    // Previous 200000/20000 was a no-op for buffer-time (that's alsasink's
    // own default!). Bump both meaningfully to give real headroom against
    // bursty delivery (channel switches, recording playback catching up)
    // instead of the driver's stock real-time-only sizing. Values in
    // microseconds.
    g_object_set(sink,
                 "buffer-time", (gint64)1000000, // 1s
                 "latency-time", (gint64)40000,  // 40ms
                 nullptr);
  }

  if (queue) {
    // Elastic, time-bounded buffer ahead of the sink: absorbs bursts from
    // VDR/decodebin without blocking the appsrc thread, and drops the
    // *oldest* data instead of overflowing ALSA's ring buffer if a burst
    // is sustained rather than momentary.
    g_object_set(queue,
                 "max-size-time", (guint64)(500 * GST_MSECOND),
                 "max-size-bytes", (guint)0,
                 "max-size-buffers", (guint)0,
                 "leaky", 2, // GST_QUEUE_LEAK_DOWNSTREAM: drop oldest on overrun
                 nullptr);
  }

  struct { const char *name; GstElement *elem; } required[] = {
    { "appsrc",         audio.appsrc },
    { "aacparse",       parse },
    { "decodebin",      decode },
    { "audioconvert",   convert },
    { "audioresample",  resample },
    { "queue",          queue },
    { *audioSinkName,   sink },
  };
  bool missing = false;
  for (auto &r : required) {
    if (!r.elem) {
      esyslog("gstoutput: GStreamer element factory '%s' not found — "
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
    GstCaps *caps = gst_pad_get_current_caps(pad);
    gchar *capsStr = caps ? gst_caps_to_string(caps) : g_strdup("(no caps yet)");
    isyslog("gstoutput: audio decodebin pad-added, caps=%s", capsStr);
    g_free(capsStr);
    if (caps) gst_caps_unref(caps);
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

// Turns a raw 33-bit PES PTS into a monotonically increasing 90kHz value
// relative to a shared baseline (the first PTS observed on *either* the
// video or audio stream). Handles the ~26.5h wraparound of the 33-bit
// counter. Both streams must go through the same ptsBaseline90k so that
// their resulting running times line up for A/V sync.
GstClockTime cGstDevice::UnwrapAndOffsetPts(cPtsUnwrap &State, int64_t RawPts33)
{
  cMutexLock lock(&ptsMutex);

  if (State.last < 0) {
    State.extended = RawPts33;
  } else {
    int64_t diff = RawPts33 - State.last;
    if (diff < -PTS_HALF)
      diff += (PTS_MASK + 1); // forward wrap
    else if (diff > PTS_HALF)
      diff -= (PTS_MASK + 1); // backward discontinuity (e.g. channel switch)
    State.extended += diff;
  }
  State.last = RawPts33;

  if (ptsBaseline90k < 0)
    ptsBaseline90k = State.extended;

  int64_t rel90k = State.extended - ptsBaseline90k;
  if (rel90k < 0)
    rel90k = 0; // the other stream's PTS arrived first and set an earlier baseline

  return gst_util_uint64_scale(static_cast<guint64>(rel90k), GST_SECOND, 90000);
}

// Gives both pipelines the same GstClock instance and the same base_time,
// which is required for two independent pipelines to agree on "running
// time" and therefore stay in sync. Without this, each pipeline picks its
// own base_time on the PAUSED->PLAYING transition and audio/video will
// drift by whatever startup jitter existed between the two transitions.
void cGstDevice::SyncPipelineClocks(void)
{
  if (!sharedClock)
    sharedClock = gst_system_clock_obtain();

  // Only share the clock *instance* between the two pipelines. We
  // deliberately no longer force an explicit base_time here: doing so
  // fights GStreamer's own automatic latency-compensated base_time
  // assignment on the PAUSED->PLAYING transition (gst_bin_do_latency()),
  // which is the most likely explanation for kmssink's "too late, buffers
  // dropped" warnings we were seeing - our forced base_time didn't leave
  // room for the pipeline's actual (VA-API decode + queues + compositor)
  // latency at all. Sharing just the clock instance still gives both
  // pipelines a common time reference; each one computes its own
  // correctly-latency-adjusted base_time against it when it goes PLAYING.
  gst_pipeline_use_clock(GST_PIPELINE(video.pipeline), sharedClock);
  gst_pipeline_use_clock(GST_PIPELINE(audio.pipeline), sharedClock);
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
      // Only log state changes of the pipelines themselves, not every
      // internal element, to keep this readable.
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

// Called from cPlugin::MainThreadHook(). See the comment in gstdevice.h -
// gst_bus_add_watch() needs a running GLib main loop to ever invoke its
// callback, which VDR doesn't provide, so we drain both buses manually
// instead. gst_bus_pop() is non-blocking (returns nullptr immediately if
// there's nothing pending), so this is safe to call frequently.
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
}

void cGstDevice::Shutdown(void)
{
  cMutexLock lock(&mutex);
  if (!initialized)
    return;

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

  if (sharedClock) {
    gst_object_unref(sharedClock);
    sharedClock = nullptr;
  }
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
  cMutexLock lock(&mutex);

  // NOTE: we deliberately do NOT send flush-start/flush-stop events
  // directly to the running appsrc elements here anymore. Doing so
  // triggered "Internal data stream error" from gst_base_src_loop() /
  // gst_queue_handle_sink_event() (observed inside vaapidecodebin's
  // internal queue) - appsrc manages its own flush state internally, and
  // injecting raw flush events into a live streaming thread this way is
  // not the supported way to reset it. Our own PTS baseline / TS-to-PES /
  // PAT-PMT resets below are enough for a clean channel switch; stale
  // buffered data downstream simply gets played out or overwritten by the
  // new stream's own discontinuities.

  {
    cMutexLock ptsLock(&ptsMutex);
    ptsBaseline90k = -1;
    videoPtsState = cPtsUnwrap();
    audioPtsState = cPtsUnwrap();
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
  // Push a single I-frame through the video appsrc and hold on PAUSED.
  PlayVideo(Data, Length);
  gst_element_set_state(video.pipeline, GST_STATE_PAUSED);
}

int cGstDevice::PlayVideo(const uchar *Data, int Length)
{
  static long callCount = 0;
  
  /*
  if (callCount == 0)
    isyslog("gstoutput: PlayVideo() called for the first time (Length=%d, first bytes=%02x %02x %02x %02x)",
            Length, Length > 0 ? Data[0] : 0, Length > 1 ? Data[1] : 0,
            Length > 2 ? Data[2] : 0, Length > 3 ? Data[3] : 0);
  if (++callCount % 200 == 0)
    isyslog("gstoutput: PlayVideo() called %ld times so far", callCount);
  */
   

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

  if (havePts)
    GST_BUFFER_PTS(buf) = UnwrapAndOffsetPts(videoPtsState, rawPts90k);
  else
    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE; // e.g. continuation packet without its own PES header

  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(video.appsrc), buf);
  if (ret != GST_FLOW_OK) {
    esyslog("gstoutput: video appsrc push failed (%d)", ret);
    return 0;
  }
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

  if (havePts)
    GST_BUFFER_PTS(buf) = UnwrapAndOffsetPts(audioPtsState, rawPts90k);
  else
    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;

  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(audio.appsrc), buf);
  if (ret != GST_FLOW_OK) {
    esyslog("gstoutput: audio appsrc push failed (%d)", ret);
    return 0;
  }
  return Length;
}

// VDR's live TV and TS-based recording playback deliver raw 188-byte TS
// packets here (not ready-made PES packets), via cDvbPlayer/cTransfer ->
// cDevice::PlayTs(). We track PAT/PMT ourselves (via the protected
// PatPmtParser() accessor) to learn the current video/audio PIDs, and
// reassemble each stream's PES packets with cTsToPes - mirroring the
// exact pattern VDR itself uses internally in cDevice::StillPicture():
// on TsPayloadStart(), drain whatever PES data has accumulated so far,
// Reset(), *then* start accumulating the new packet.
int cGstDevice::PlayTs(const uchar *Data, int Length, bool VideoOnly)
{
  if (!Data || Length <= 0)
    return 0;

  static long tsCallCount = 0;

  /*
  if (tsCallCount == 0)
    isyslog("gstoutput: PlayTs() called for the first time (Length=%d, VideoOnly=%d)", Length, VideoOnly);
  if (++tsCallCount % 500 == 0)
    isyslog("gstoutput: PlayTs() called %ld times so far", tsCallCount);
  */ 
  
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
    return pos90k; // no PES PTS observed yet, best effort

  // Re-express in the original broadcast PTS domain (mod 2^33), since
  // that's what callers comparing against raw stream PTS values expect.
  return (pos90k + ptsBaseline90k) & PTS_MASK;
}
