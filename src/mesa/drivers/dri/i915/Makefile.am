

# Copyright © 2012 Matt Turner <mattst88@gmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

include Makefile.sources

AM_CFLAGS = \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src/ \
	-I$(top_srcdir)/src/mapi \
	-I$(top_srcdir)/src/mesa/ \
	-I$(top_srcdir)/src/mesa/drivers/dri/common \
	-I$(top_srcdir)/src/mesa/drivers/dri/intel/server \
	-I$(top_builddir)/src/mesa/drivers/dri/common \
	$(DEFINES) \
	$(VISIBILITY_CFLAGS) \
	$(INTEL_CFLAGS)

dridir = $(DRI_DRIVER_INSTALL_DIR)
dri_LTLIBRARIES = i915_dri.la

i915_dri_la_SOURCES = $(i915_FILES)
i915_dri_la_LDFLAGS = $(DRI_DRIVER_LDFLAGS)
i915_dri_la_LIBADD = \
	../common/libdricommon.la \
	$(DRI_LIB_DEPS) \
	$(INTEL_LIBS)

# Provide compatibility with scripts for the old Mesa build system for
# a while by putting a link to the driver into /lib of the build tree.
all-local: i915_dri.la
	$(MKDIR_P) $(top_builddir)/$(LIB_DIR);
	ln -f .libs/i915_dri.so $(top_builddir)/$(LIB_DIR)/i915_dri.so;
