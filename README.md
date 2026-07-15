# vdr-plugin-gstoutput

VDR output plugin using **GStreamer** as the rendering backend, as an
alternative to the existing mpv/SDL2 output plugin.

## Architecture

```
                 ┌────────────────────────────────────────────┐
 VDR TS/PES  ──► │ appsrc (video) → h264parse → decodebin ──┐  │
                 │                                          ▼  │
                 │                              videoconvert   │
                 │                                          ▼  │
                 │  appsrc (OSD, BGRA) → videoconvert ──► compositor ──► kmssink/waylandsink
                 └────────────────────────────────────────────┘

                 ┌────────────────────────────────────────────┐
 VDR TS/PES  ──► │ appsrc (audio) → aacparse → decodebin →     │
                 │  audioconvert → audioresample → alsasink    │
                 └────────────────────────────────────────────┘
```

- `cGstDevice` (`gstdevice.h/.cpp`) is the `cDevice` implementation. VDR's
  `PlayVideo()`/`PlayAudio()` push elementary stream data into the
  respective `appsrc`. Everything downstream (parsing, decoding, hardware
  sink) is standard GStreamer, auto-plugged via `decodebin`.
- `cGstOsd` / `cGstOsdProvider` (`gstosd.h/.cpp`) render VDR's OSD bitmaps
  into a BGRA buffer and push it through a dedicated `appsrc` into a
  second `compositor` input pad (`zorder=1`), so OSD is alpha-blended on
  top of the video entirely inside the GStreamer pipeline — no X11,
  Wayland compositor, or SDL2 layer required.
- `compositor` was chosen over `glvideomixer` to keep this working on
  fbdev/KMS-only targets without a GL context; swap it for
  `glvideomixer` + `waylandsink` if your BSP has a working EGL/GLES
  stack you'd rather use.

## Known gaps / TODO before production use

This is a working skeleton, not a finished driver — same starting point
as the earlier mpv/SDL2 and REST API plugins before hardening:

1. **PTS extraction**: `PlayVideo()`/`PlayAudio()` currently push buffers
   with `GST_CLOCK_TIME_NONE`. Extract the real PTS from the incoming
   PES/TS packet headers and set `GST_BUFFER_PTS()` so A/V sync and
   `GetSTC()` are meaningful.
2. **Demuxer choice**: the video branch assumes raw H.264 ES
   (`h264parse`). If VDR hands you full TS packets instead, insert
   `tsdemux` (or `tsparse ! tsdemux`) ahead of `h264parse`, and switch
   `aacparse` for whatever codec parser matches your broadcast audio
   (`ac3parse`, `mpegaudioparse`, etc.) — possibly detected dynamically
   per channel.
3. **Screen size for OSD**: `cGstOsd` hardcodes 1920x1080; query the
   negotiated caps from the video sink (or `kmssink`'s connector mode)
   instead.
4. **Trick speed**: `TrickSpeed()` sends a rate-only seek event; pair
   this with `HasIBPTrickSpeed()` (already returns `true`) so VDR only
   delivers I-frames during FF/RW, or decodebin will choke trying to
   decode P/B frames out of order.
5. **Multi-instance**: if you need PIP or multiple tuners rendered
   simultaneously, this is where GStreamer's advantage over mpv shows —
   just instantiate additional `cGstDevice`s, each with its own
   pipeline/compositor.

## Build

Requires (Yocto recipe names in brackets):
- `gstreamer1.0` core + `gstreamer1.0-plugins-base` (`packagegroup-gstreamer1.0`)
- `gstreamer1.0-plugins-good` (for `compositor`, parsers)
- `gstreamer1.0-plugins-bad` (`kmssink`, if not already in -good on your version)
- `gstreamer1.0-libav` if you need software decode fallback

```sh
make VDRDIR=/path/to/vdr/source
```

For BitBake, add `gstreamer1.0`, `gstreamer1.0-plugins-base`,
`gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad` to `DEPENDS` in
the plugin's recipe (see `gstoutput_git.bb` example alongside this
plugin) — same pattern already used for the `libmali`/`vdr-rectools`
recipes for `virtual/egl` and CLI/web split packages.

## Runtime options

```
vdr -P"gstoutput -v kmssink -a alsasink -c HDMI-A-1"
```

or in `plugins.conf`:

```
gstoutput -v kmssink -a alsasink -c HDMI-A-1
```

`VideoSink`, `AudioSink`, `Connector` are also read from
`setup.conf` under `[gstoutput]` if set via a future setup menu page
(`SetupMenu()` currently returns `nullptr` — same TODO as the setup page
that was still pending on the mpv/SDL2 plugin).
