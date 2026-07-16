#include "gstsetup.h"
#include <vdr/tools.h>
#include <string.h>

// Common choices; "kmssink"/"waylandsink" for DRM-direct or Wayland output,
// "autovideosink"/"autoaudiosink" as a safe fallback while bringing up a
// new target. Free-form sinks (e.g. with extra properties) still have to
// go through plugins.conf - see gstconfig.h.
const char *cMenuSetupGst::videoSinkOptions[] = { "kmssink", "waylandsink", "autovideosink" };
const char *cMenuSetupGst::audioSinkOptions[] = { "alsasink", "pulsesink", "autoaudiosink" };
const int   cMenuSetupGst::numVideoSinkOptions = sizeof(videoSinkOptions) / sizeof(*videoSinkOptions);
const int   cMenuSetupGst::numAudioSinkOptions = sizeof(audioSinkOptions) / sizeof(*audioSinkOptions);

int cMenuSetupGst::IndexOf(const char *Options[], int NumOptions, const char *Value)
{
  for (int i = 0; i < NumOptions; i++) {
    if (!strcmp(Options[i], Value))
      return i;
  }
  return 0; // fall back to the first option if the stored value is a custom
            // string not in our dropdown list (e.g. hand-edited plugins.conf)
}

cMenuSetupGst::cMenuSetupGst(void)
{
  videoSinkIndex = IndexOf(videoSinkOptions, numVideoSinkOptions, *GstConfig.videoSink);
  audioSinkIndex = IndexOf(audioSinkOptions, numAudioSinkOptions, *GstConfig.audioSink);
  strn0cpy(connector, *GstConfig.connector, sizeof(connector));

  Add(new cMenuEditStraItem(tr("Video sink"), &videoSinkIndex, numVideoSinkOptions, videoSinkOptions));
  Add(new cMenuEditStraItem(tr("Audio sink"), &audioSinkIndex, numAudioSinkOptions, audioSinkOptions));
  Add(new cMenuEditStrItem(tr("DRM connector (empty = auto)"), connector, sizeof(connector)));
  Add(new cOsdItem(tr("Note: changes take effect after restarting VDR"), osUnknown, false));
}

void cMenuSetupGst::Store(void)
{
  GstConfig.videoSink = videoSinkOptions[videoSinkIndex];
  GstConfig.audioSink = audioSinkOptions[audioSinkIndex];
  GstConfig.connector = connector;

  SetupStore("VideoSink", *GstConfig.videoSink);
  SetupStore("AudioSink", *GstConfig.audioSink);
  SetupStore("Connector", *GstConfig.connector);
}
