#ifndef VDR_GST_DEVICE_H
#define VDR_GST_DEVICE_H

#include <vdr/device.h>
#include <vdr/thread.h>
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

class cGstDevice : public cDevice {
private:
  cString videoSinkName;
  cString audioSinkName;
  cString connectorName;

  cGstStream video;
  cGstStream audio;

  GstElement *compositor = nullptr; // mixes video + OSD overlay
  GstElement *osdAppsrc  = nullptr; // OSD bitmap feed, see cGstOsdProvider

  cMutex mutex;
  bool playing = false;
  bool initialized = false;

  double currentPts = 0.0;

  bool BuildVideoPipeline(void);
  bool BuildAudioPipeline(void);
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

  virtual bool HasIBPTrickSpeed(void) { return true; }
  virtual int64_t GetSTC(void);

public:
  cGstDevice(const char *VideoSink, const char *AudioSink, const char *Connector);
  virtual ~cGstDevice();

  bool Init(void);
  void Shutdown(void);
  bool IsPlaying(void) const { return playing; }

  // Used by cGstOsdProvider to push a rendered OSD frame into the mixer.
  GstElement *OsdAppSrc(void) { return osdAppsrc; }
  GstElement *Compositor(void) { return compositor; }
};

#endif
