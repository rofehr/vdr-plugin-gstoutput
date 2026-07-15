#
# Makefile for the VDR GStreamer output plugin
#
# The Makefile follows VDR's standard plugin build conventions so it
# drops straight into a VDR PLUGINS/src/gstoutput directory and builds
# via VDR's top-level `make plugins`, as well as under the BitBake
# recipe for out-of-tree building (see gstoutput_%.bb pattern used
# elsewhere in this environment for vdr-rectools / miniDLNA style
# recipes).

PLUGIN = gstoutput

### The version number of this plugin (taken from the main source file):
VERSION = $(shell grep 'VERSION *=' gstoutput.c | awk '{ print $$4 }' | sed -e 's/[";]//g')

### The C++ compiler and options:
CXX      ?= g++
CXXFLAGS ?= -g -O2 -Wall -Wextra -Wno-parentheses -Wno-unused-parameter -fPIC

PKGCFG = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),$(shell pkg-config --variable=$(1) vdr || pkg-config --variable=$(1) ../../../vdr.pc))
LIBDIR = $(call PKGCFG,libdir)

-include $(VDRDIR)/Make.global

### The version number of VDR's plugin API:
APIVERSION = $(call PKGCFG,apiversion)

### GStreamer flags via pkg-config (provided by the gstreamer1.0-*-dev /
### Yocto libgstreamer1.0 + libgstapp1.0 recipes):
GSTPKGS     = gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
GSTCFLAGS   = $(shell pkg-config --cflags $(GSTPKGS))
GSTLIBS     = $(shell pkg-config --libs $(GSTPKGS))

### The name of the distribution archive:
ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)

### Includes and Defines (add further entries here):
INCLUDES += -I$(VDRDIR)/include $(GSTCFLAGS)
DEFINES  += -D_GNU_SOURCE -DPLUGIN_NAME_I18N='"$(PLUGIN)"'

### The object files (add further files here):
OBJS = gstoutput.o gstdevice.o gstosd.o

### The main target:
all: libvdr-$(PLUGIN).so

### Implicit rules:
%.o: %.c
	$(CXX) $(CXXFLAGS) -c $(DEFINES) $(INCLUDES) -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $(DEFINES) $(INCLUDES) -o $@ $<

### Dependencies:
MAKEDEP = $(CXX) -MM -MG
DEPFILE = .dependencies
$(DEPFILE): Makefile
	@$(MAKEDEP) $(DEFINES) $(INCLUDES) gstoutput.c gstdevice.cpp gstosd.cpp > $@

-include $(DEPFILE)

### Targets:
libvdr-$(PLUGIN).so: $(OBJS)
	$(CXX) $(CXXFLAGS) -shared $(OBJS) $(GSTLIBS) -o $@
	@cp --remove-destination $@ $(LIBDIR)/$@.$(APIVERSION)

install-lib: libvdr-$(PLUGIN).so
	install -d $(DESTDIR)$(LIBDIR)
	install -m755 libvdr-$(PLUGIN).so $(DESTDIR)$(LIBDIR)/libvdr-$(PLUGIN).so.$(APIVERSION)
		
install: install-lib

dist: clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo Distribution package created as $(PACKAGE).tgz

clean:
	@-rm -f $(OBJS) $(DEPFILE) *.so *.tgz core* *~

.PHONY: all install dist clean
