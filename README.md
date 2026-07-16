# vdr-plugin-gstoutput

VDR output plugin using **GStreamer** as the rendering backend, as an
alternative to the existing mpv/SDL2 output plugin.

## Architecture

```
VDR (live TV / TS recording playback)
        │
        ▼
  cDevice::PlayTs(Data, Length, VideoOnly)          <- raw 188-byte TS packets
        │
        │  own PAT/PMT parsing (ownPatPmtParser) to learn current Vpid/Apid
        │  per-PID reassembly via cTsToPes (VDR's own TS→PES helper),
        │  flushed on TsPayloadStart() exactly like cDevice::StillPicture() does
        ▼
  PlayVideo(pes, len)                    PlayAudio(pes, len, pid)
        │                                       │
        │  ExtractPesPts(): parse the PES        │  same PES PTS extraction
        │  header, unwrap the 33-bit/90kHz        │
        │  counter, offset against a shared        │
        │  baseline so video+audio agree on         │
        │  t=0 (UnwrapAndOffsetPts())                │
        ▼                                       ▼
┌───────────────────────────────────┐   ┌───────────────────────────────┐
│ appsrc(video) → h264parse →       │   │ appsrc(audio) → aacparse →    │
│ decodebin → videoconvert →        │   │ decodebin → audioconvert →    │
│ queue(leaky,500ms) → compositor   │   │ audioresample → queue(leaky,  │
│  ▲                          │     │   │ 500ms) → alsasink            │
│  │ appsrc(OSD,BGRA)          ▼     │   └───────────────────────────────┘
│  └→ videoconvert ──────► compositor → kmssink/waylandsink            │
└───────────────────────────────────┘
```

Both pipelines share one `GstClock` and an explicitly-synced `base_time`
(`SyncPipelineClocks()`), so their independently-computed running times
actually agree — required for A/V sync across two separate
`GstPipeline` instances.

### Key pieces

- `cGstDevice::PlayTs()` (`gstdevice.cpp`) — the real entry point for both
  live TV and TS-based recording playback. VDR hands this raw 188-byte TS
  packets; overriding it ourselves (rather than relying on `cDevice`'s
  default) was necessary because `cDevice::patPmtParser` is **private** in
  this VDR version, so we track our own `cPatPmtParser` and reassemble
  each stream's PES packets with `cTsToPes`, following the exact
  flush-then-reset-then-put pattern VDR itself uses internally in
  `cDevice::StillPicture()`.
- `ExtractPesPts()` / `UnwrapAndOffsetPts()` (`gstdevice.cpp`) — parse the
  33-bit/90kHz PTS out of each PES header (per ISO/IEC 13818-1), unwrap
  the ~26.5h wraparound, and offset both streams against one shared
  baseline (whichever stream's PTS is seen first defines running time 0)
  so video and audio timestamps are directly comparable.
- `cGstDevice::PlayVideo()`/`PlayAudio()` — push the reassembled PES
  payload into the respective `appsrc` with a real `GST_BUFFER_PTS`
  (falls back to `GST_CLOCK_TIME_NONE` for header-less continuation
  fragments).
- Elastic `queue` (leaky=downstream, ~500ms) ahead of both the video
  compositor and the audio sink — this decouples the sink's real-time,
  `sync=TRUE` blocking render loop into GStreamer's own streaming thread,
  so `gst_app_src_push_buffer()` — called synchronously from VDR's own
  receiving thread via `PlayTs()` — never blocks waiting for display/audio
  timing. Without this, VDR's *own* internal ring buffers (e.g.
  vdr-plugin-satip's RTP receive buffer) can overflow because they can't
  be drained fast enough while our thread is stuck waiting on the sink.
- `cGstOsd`/`cGstOsdProvider` (`gstosd.h/.cpp`) — render VDR's OSD bitmaps
  into a BGRA buffer and push it through a dedicated `appsrc` into a
  second `compositor` input pad (`zorder=1`), so OSD is alpha-blended on
  top of the video entirely inside the GStreamer pipeline — no X11,
  Wayland compositor, or SDL2 layer required.
- `compositor` was chosen over `glvideomixer` to keep this working on
  fbdev/KMS-only targets without a GL context; swap it for
  `glvideomixer` + `waylandsink` if your BSP has a working EGL/GLES
  stack you'd rather use.

### Diagnostics built in

Given how silent GStreamer pipeline failures can be, the following is
logged via `isyslog`/`esyslog` (grep the VDR log for `gstoutput:`):

- `PlayTs()`, `PlayVideo()`, `PlayAudio()` — logs on the first call and
  every Nth call after, so you can confirm VDR is actually feeding this
  device data at all.
- `SetPlayMode(N) called` — confirms VDR is switching this device into
  a playing mode.
- `video/audio decodebin pad-added, caps=...` — confirms `decodebin`
  successfully typefound and negotiated a format.
- `video decodebin auto-plugged: <factory-name>` (via the
  `deep-element-added` signal) — shows exactly which decoder element
  `decodebin` picked. On Intel hardware, seeing `avdec_h264` instead of
  `vah264dec`/`vaapih264dec` means it fell back to software decode —
  see the VA-API note below.
- GStreamer bus `ERROR`, `WARNING`, and pipeline-level `STATE_CHANGED`
  messages are all logged (warnings were previously swallowed —
  things like `not-negotiated` show up here now).
- Missing-element errors during pipeline construction name the exact
  GStreamer factory that couldn't be found, rather than a generic
  "something's missing".

## Known gaps / TODO before production use

1. **Audio codec assumed fixed (`aacparse`)**: the audio branch
   hardcodes an AAC parser. If a channel carries AC-3/MPEG audio instead,
   swap in `ac3parse`/`mpegaudioparse` — ideally selected dynamically per
   channel/PID rather than hardcoded.
2. **OSD screen size hardcoded**: `cGstOsd` assumes 1920x1080; query the
   negotiated caps from the video sink (or `kmssink`'s connector mode)
   instead.
3. **Trick speed**: `TrickSpeed()` sends a rate-only seek event; pair
   this with `HasIBPTrickSpeed()` (already returns `true`) so VDR only
   delivers I-frames during FF/RW, or decodebin will choke trying to
   decode P/B frames out of order.
4. **Hardware decode not guaranteed**: `decodebin` auto-plugs whatever
   decoder GStreamer ranks highest. On Intel targets this requires
   `libva` + a matching driver (`intel-media-driver` for Broadwell and
   newer, `libva-intel-driver` for older Gen <9) and the VA-API GStreamer
   plugin to be present at all — otherwise it silently falls back to
   software decode. This isn't just a performance nit: a CPU-bound
   software decoder can starve *other* system threads (observed
   concretely with vdr-plugin-satip's network receive thread, whose own
   RTP ring buffer overflowed under CPU pressure — the `deep-element-added`
   log line above is how to confirm which case you're in).
5. **Multi-instance**: if you need PIP or multiple tuners rendered
   simultaneously, this is where GStreamer's advantage over mpv shows —
   just instantiate additional `cGstDevice`s, each with its own
   pipeline/compositor.
6. **Setup menu**: `SetupMenu()` still returns `nullptr`; `VideoSink`,
   `AudioSink`, `Connector` are only configurable via command line /
   `plugins.conf` for now.

## Build

Requires (Yocto recipe names in brackets):
- `gstreamer1.0` core + `gstreamer1.0-plugins-base` (`packagegroup-gstreamer1.0`)
- `gstreamer1.0-plugins-good` (for `compositor`, `alsasink` — needs its
  `alsa` `PACKAGECONFIG` enabled, see `gstoutput_git.bb`)
- `gstreamer1.0-plugins-bad` (`kmssink`, `h264parse`)
- On Intel targets, for hardware decode: `libva`, `intel-media-driver`
  (or `libva-intel-driver` on older Gen <9 hardware), `gstreamer1.0-vaapi`
  — check what's actually present on-target with:
  ```sh
  gst-inspect-1.0 | grep -Ei 'va|vaapi'
  vainfo   # confirms the VA driver loads and lists supported codec profiles
  ```

```sh
make VDRDIR=/path/to/vdr/source
```

For BitBake, see `gstoutput_git.bb` alongside this plugin — same pattern
already used for the `libmali`/`vdr-rectools` recipes for `virtual/egl`
and CLI/web split packages.

## Runtime options

```
vdr -P"gstoutput -v kmssink -a alsasink -c HDMI-A-1"
```

or in `plugins.conf`:

```
gstoutput -v kmssink -a alsasink -c HDMI-A-1
```

`VideoSink`, `AudioSink`, `Connector` are also read from
`setup.conf` under `[gstoutput]` if set (no setup menu page to edit them
yet — see TODO above).
