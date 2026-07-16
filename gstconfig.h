#ifndef VDR_GST_CONFIG_H
#define VDR_GST_CONFIG_H

#include <vdr/tools.h>

// Single shared source of truth for this plugin's persistent configuration.
// Populated from command-line args / setup.conf at startup
// (cPluginGstOutput::ProcessArgs()/SetupParse()), read by cGstDevice when
// the pipeline is built, and editable via cMenuSetupGst (gstsetup.h/.cpp).
//
// NOTE: changing VideoSink/AudioSink/Connector here only takes effect on
// the next VDR restart - rebuilding a live GStreamer pipeline with a
// different sink element while VDR is running is out of scope for this
// plugin (same restart requirement as most other hardware-sink VDR output
// plugins).
struct cGstConfig {
  cString videoSink = "kmssink";
  cString audioSink = "alsasink";
  cString connector = "";  // empty = let kmssink auto-pick the connector
};

extern cGstConfig GstConfig;

#endif
