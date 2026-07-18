#include "gstosd.h"
#include <gst/app/gstappsrc.h>
#include <vdr/tools.h>
#include <string.h>

cGstOsdProvider::cGstOsdProvider(cGstDevice *Device)
: device(Device)
{
}

cOsd *cGstOsdProvider::CreateOsd(int Left, int Top, uint Level)
{
  return new cGstOsd(device, Left, Top, Level);
}

cGstOsd::cGstOsd(cGstDevice *Device, int Left, int Top, uint Level)
: cOsd(Left, Top, Level), device(Device)
{
  screenWidth  = 1920; // TODO: query from the negotiated video caps / kmssink mode
  screenHeight = 1080;
}

cGstOsd::~cGstOsd()
{
}

void cGstOsd::SetActive(bool Active)
{
  cOsd::SetActive(Active);
  if (!Active && device && device->OsdAppSrc()) {
    // Push a fully transparent frame to clear the overlay when OSD closes.
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, (gsize)screenWidth * screenHeight * 4, nullptr);
    gst_buffer_memset(buf, 0, 0, gst_buffer_get_size(buf));
    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
    device->PushOsdBuffer(buf);
  }
}

// Composites all dirty bitmaps of this cOsd into a single BGRA frame and
// pushes it to the OSD appsrc. VDR's cBitmap already stores 32-bit ARGB
// pixels, so this is mostly a straight copy plus format/byte-order fix-up.
void cGstOsd::Flush(void)
{
  if (!Active() || !device || !device->OsdAppSrc())
    return;

  cBitmap *bitmap;
  for (int i = 0; (bitmap = GetBitmap(i)) != nullptr; i++) {
    int dirtyX1, dirtyY1, dirtyX2, dirtyY2;
    if (!bitmap->Dirty(dirtyX1, dirtyY1, dirtyX2, dirtyY2))
      continue;

    int w = bitmap->Width();
    int h = bitmap->Height();

    GstBuffer *buf = gst_buffer_new_allocate(nullptr, (gsize)screenWidth * screenHeight * 4, nullptr);
    gst_buffer_memset(buf, 0, 0, gst_buffer_get_size(buf)); // transparent background

    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    uint32_t *dst = reinterpret_cast<uint32_t *>(map.data);

    int destLeft = Left() + bitmap->X0();
    int destTop  = Top()  + bitmap->Y0();

    for (int y = 0; y < h; y++) {
      int dy = destTop + y;
      if (dy < 0 || dy >= screenHeight)
        continue;
      for (int x = 0; x < w; x++) {
        int dx = destLeft + x;
        if (dx < 0 || dx >= screenWidth)
          continue;
        // cBitmap::GetColor() returns ARGB; GStreamer's BGRA expects
        // little-endian B,G,R,A byte order in memory.
        tColor argb = bitmap->GetColor(x, y);
        uint8_t a = (argb >> 24) & 0xFF;
        uint8_t r = (argb >> 16) & 0xFF;
        uint8_t g = (argb >> 8)  & 0xFF;
        uint8_t b =  argb        & 0xFF;
        dst[dy * screenWidth + dx] = (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
    gst_buffer_unmap(buf, &map);

    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE; // re-timestamped by do-timestamp=TRUE
    device->PushOsdBuffer(buf);

    bitmap->Clean();
  }
}
