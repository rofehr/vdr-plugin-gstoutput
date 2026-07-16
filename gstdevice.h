#ifndef VDR_GST_DEVICE_H
#define VDR_GST_DEVICE_H

#include <vdr/device.h>
#include <vdr/thread.h>
#include <vdr/remux.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

// One appsrc-fed sub-pipeline per elementary stream type.
struct cGstStream {
  GstElement *pipeline   = nullptr;
  GstElement *appsrc     = nullptr;
  GstElement *parser     = nullptr;
  GstElement *decoder    = nullptr; // decodebin, auto-plugged
  GstElement *sink       = nullptr;
  GstBus     *bus        = nullptr;
  guint       watchId    = 0;
  bool        eos        = false;
};

// Tracks 33-bit (90kHz) PES PTS unwrap state for one elementary stream, so
// that wraparounds (every ~26.5h) and normal 90kHz counting turn into a
// monotonically increasing 64-bit value.
struct cPtsUnwrap {
  int64_t last     = -1; // last raw 33-bit PTS seen
  int64_t extended = -1; // unwrapped, ever-increasing 90kHz counter
};

class cGstDevice : public cDevice {
private:
  cString videoSinkName;
  cString audioSinkName;
  cString connectorName;

  cGstStream video;
  cGstStream audio;

  // TS -> PES reassembly, driven from our PlayTs() override (VDR's live TV
  // and TS-based recording playback deliver raw 188-byte TS packets here,
  // not ready-made PES packets - see PlayTs()).
  cTsToPes tsToPesVideo;
  cTsToPes tsToPesAudio;
  cPatPmtParser ownPatPmtParser; // cDevice::patPmtParser is private in this VDR version, so we track our own

  GstElement *compositor = nullptr; // mixes video + OSD overlay
  GstElement *osdAppsrc  = nullptr; // OSD bitmap feed, see cGstOsdProvider

  cMutex mutex;
  bool playing = false;
  bool initialized = false;

  double currentPts = 0.0;

  // --- PTS handling (see PlayVideo/PlayAudio) ---
  GstClock *sharedClock  = nullptr; // one clock instance for both pipelines, so
                                     // their running-time bases actually agree
  cMutex    ptsMutex;
  int64_t   ptsBaseline90k = -1;    // first PTS seen (video or audio) -> running time 0
  cPtsUnwrap videoPtsState;
  cPtsUnwrap audioPtsState;

  GstClockTime UnwrapAndOffsetPts(cPtsUnwrap &State, int64_t RawPts33);
  void         SyncPipelineClocks(void);

  bool BuildVideoPipeline(void);
  bool BuildAudioPipeline(void);
  void PushInitialOsdFrame(void);
  static gboolean BusCallback(GstBus *bus, GstMessage *msg, gpointer data);

protected:
  virtual bool CanReplay(void) const { return true; }
  virtual bool SetPlayMode(ePlayMode PlayMode);
  virtual void TrickSpeed(int Speed, bool Forward);
  virtual void Clear(void);
  virtual void Play(void);
  virtual void Freeze(void);
  virtual void Mute(void);
  virtual void StillPicture(const uchar *Data, int Length);

  virtual int PlayVideo(const uchar *Data, int Length);
  virtual int PlayAudio(const uchar *Data, int Length, uchar Id);
  virtual int PlayTs(const uchar *Data, int Length, bool VideoOnly = false);

  virtual bool HasIBPTrickSpeed(void) { return true; }
  virtual int64_t GetSTC(void);

public:
  cGstDevice(const char *VideoSink, const char *AudioSink, const char *Connector);
  virtual ~cGstDevice();

  bool Init(void);
  void Shutdown(void);
  bool IsPlaying(void) const { return playing; }

  // Let an independent playback session (e.g. the media player, see
  // gstplayer.h) temporarily take over the display/audio hardware: our
  // own video pipeline holds the DRM plane via kmssink as long as it's
  // not in NULL state, which would conflict with a second sink trying to
  // grab the same plane.
  void SuspendDisplay(void);
  void ResumeDisplay(void);

  // Used by cGstOsdProvider to push a rendered OSD frame into the mixer.
  GstElement *OsdAppSrc(void) { return osdAppsrc; }
  GstElement *Compositor(void) { return compositor; }
};

#endif
