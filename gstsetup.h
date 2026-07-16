#ifndef VDR_GST_SETUP_H
#define VDR_GST_SETUP_H

#include <vdr/menuitems.h>
#include "gstconfig.h"

// VDR Setup > Plugins > gstoutput menu page. Lets the user pick the video
// and audio sink element and an optional DRM connector name without
// editing plugins.conf by hand. See gstconfig.h for the restart caveat.
class cMenuSetupGst : public cMenuSetupPage {
private:
  static const char *videoSinkOptions[];
  static const char *audioSinkOptions[];
  static const int   numVideoSinkOptions;
  static const int   numAudioSinkOptions;

  int  videoSinkIndex;
  int  audioSinkIndex;
  char connector[32];

  static int IndexOf(const char *Options[], int NumOptions, const char *Value);

protected:
  virtual void Store(void);

public:
  cMenuSetupGst(void);
};

#endif
