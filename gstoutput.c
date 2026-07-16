/*
 * gstoutput.c: VDR output plugin using GStreamer as the rendering backend
 *
 * Architecture:
 *   - cPluginGstOutput registers a single cGstDevice at startup.
 *   - cGstDevice feeds elementary/PES data received from VDR into a
 *     GStreamer pipeline built around appsrc -> parsebin/decodebin ->
 *     video/audio sinks (kmssink/waylandsink + alsasink/pulsesink).
 *   - OSD is realized via cGstOsdProvider, which renders VDR's OSD bitmaps
 *     into a GstBuffer that is composited on top of the video using
 *     compositor/glvideomixer, so no separate windowing system is needed.
 *
 * This mirrors the structure of the existing mpv/SDL2 output plugin, but
 * swaps the playback engine for GStreamer, which is preferable when you
 * need multiple simultaneous pipelines, hardware sinks with vendor BSP
 * integration (e.g. imxvideosink, kmssink), or complex filter graphs.
 */

#include <vdr/plugin.h>
#include <vdr/device.h>
#include <vdr/osd.h>
#include <getopt.h>

#include "gstdevice.h"
#include "gstosd.h"
#include "gstconfig.h"
#include "gstsetup.h"

static const char *VERSION        = "0.1.0";
static const char *DESCRIPTION    = "GStreamer based output device";
static const char *MAINMENUENTRY  = NULL;

cGstConfig GstConfig; // single shared instance, see gstconfig.h

class cPluginGstOutput : public cPlugin {
private:
  cGstDevice *device = nullptr;

public:
  cPluginGstOutput() = default;
  virtual ~cPluginGstOutput() = default;

  virtual const char *Version(void)      { return VERSION; }
  virtual const char *Description(void)  { return DESCRIPTION; }
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Initialize(void);
  virtual bool Start(void);
  virtual void Stop(void);
  virtual void Housekeeping(void) {}
  virtual void MainThreadHook(void) { if (device) device->PollBus(); }
  virtual cString Active(void);
  virtual time_t WakeupTime(void) { return 0; }
  virtual const char *MainMenuEntry(void) { return MAINMENUENTRY; }
  virtual cOsdObject *MainMenuAction(void) { return nullptr; }
  virtual cMenuSetupPage *SetupMenu(void);
  virtual bool SetupParse(const char *Name, const char *Value);
  virtual bool Service(const char *Id, void *Data = nullptr);
  virtual const char **SVDRPHelpPages(void) { return nullptr; }
  virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode) { return nullptr; }
};

const char *cPluginGstOutput::CommandLineHelp(void)
{
  return
    "  -v CMD,   --videosink=CMD  GStreamer video sink element (default: kmssink)\n"
    "  -a CMD,   --audiosink=CMD  GStreamer audio sink element (default: alsasink)\n"
    "  -c NAME,  --connector=NAME DRM connector name for kmssink (e.g. HDMI-A-1)\n";
}

bool cPluginGstOutput::ProcessArgs(int argc, char *argv[])
{
  static const struct option long_options[] = {
    { "videosink", required_argument, nullptr, 'v' },
    { "audiosink", required_argument, nullptr, 'a' },
    { "connector", required_argument, nullptr, 'c' },
    { nullptr,     0,                 nullptr,  0  }
  };

  int c;
  while ((c = getopt_long(argc, argv, "v:a:c:", long_options, nullptr)) != -1) {
    switch (c) {
      case 'v': GstConfig.videoSink = optarg; break;
      case 'a': GstConfig.audioSink = optarg; break;
      case 'c': GstConfig.connector = optarg; break;
      default:  return false;
    }
  }
  return true;
}

bool cPluginGstOutput::Initialize(void)
{
  // gst_init() is deferred to Start() so ProcessArgs() / SetupParse()
  // options are already known (sink names, connector, etc.)
  device = new cGstDevice(*GstConfig.videoSink, *GstConfig.audioSink, *GstConfig.connector);
  return device != nullptr;
}

bool cPluginGstOutput::Start(void)
{
  if (!device)
    return false;
  if (!device->Init()) {
    esyslog("gstoutput: failed to initialize GStreamer pipeline");
    return false;
  }
  cOsdProvider::Shutdown();
  new cGstOsdProvider(device);
  const char *connStr = *GstConfig.connector;
  isyslog("gstoutput: plugin started (video=%s audio=%s connector=%s)",
          *GstConfig.videoSink, *GstConfig.audioSink, (connStr && *connStr) ? connStr : "auto");
  return true;
}

void cPluginGstOutput::Stop(void)
{
  if (device) {
    device->Shutdown();
  }
}

cString cPluginGstOutput::Active(void)
{
  if (device && device->IsPlaying())
    return "gstoutput: playback active";
  return nullptr;
}

bool cPluginGstOutput::SetupParse(const char *Name, const char *Value)
{
  if (!strcasecmp(Name, "VideoSink"))      GstConfig.videoSink = Value;
  else if (!strcasecmp(Name, "AudioSink")) GstConfig.audioSink = Value;
  else if (!strcasecmp(Name, "Connector")) GstConfig.connector = Value;
  else return false;
  return true;
}

cMenuSetupPage *cPluginGstOutput::SetupMenu(void)
{
  return new cMenuSetupGst();
}

bool cPluginGstOutput::Service(const char *Id, void *Data)
{
  return false;
}

VDRPLUGINCREATOR(cPluginGstOutput); // Don't touch this!
