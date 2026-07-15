#ifndef VDR_GST_OSD_H
#define VDR_GST_OSD_H

#include <vdr/osd.h>
#include <gst/gst.h>
#include "gstdevice.h"

// Renders VDR's cOsd bitmap buffers to a BGRA GstBuffer and pushes it
// into the device's compositor OSD sink pad, so OSD is alpha-blended
// on top of the decoded video by GStreamer itself (no extra window
// manager / SDL2 layer needed on top of GStreamer).
class cGstOsd : public cOsd {
private:
  cGstDevice *device;
  int screenWidth, screenHeight;

public:
  cGstOsd(cGstDevice *Device, int Left, int Top, uint Level);
  virtual ~cGstOsd();
  virtual void Flush(void);
  virtual void SetActive(bool Active);
};

class cGstOsdProvider : public cOsdProvider {
private:
  cGstDevice *device;

public:
  cGstOsdProvider(cGstDevice *Device);
  virtual cOsd *CreateOsd(int Left, int Top, uint Level);
  virtual bool ProvidesTrueColor(void) { return true; }
};

#endif
