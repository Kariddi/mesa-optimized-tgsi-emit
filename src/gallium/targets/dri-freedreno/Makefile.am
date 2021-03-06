# Copyright © 2012 Intel Corporation
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
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	$(GALLIUM_CFLAGS) \
	$(PTHREAD_CFLAGS) \
	$(LIBDRM_CFLAGS)
AM_CPPFLAGS = \
	-I$(top_srcdir)/src/gallium/drivers \
	-I$(top_srcdir)/src/gallium/winsys \
	-I$(top_srcdir)/src/mesa \
	-I$(top_srcdir)/src/mapi \
	-I$(top_builddir)/src/mesa/drivers/dri/common \
	-DGALLIUM_RBUG \
	-DGALLIUM_TRACE \
	-DGALLIUM_NOOP

dridir = $(DRI_DRIVER_INSTALL_DIR)
dri_LTLIBRARIES = kgsl_dri.la msm_dri.la

COMMON_LDFLAGS = $(DRI_DRIVER_LDFLAGS)

COMMON_LIBADD = \
	$(top_builddir)/src/mesa/drivers/dri/common/libdricommon.la \
	$(top_builddir)/src/mesa/libmesagallium.la \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/gallium/state_trackers/dri/drm/libdridrm.la \
	$(top_builddir)/src/gallium/winsys/freedreno/drm/libfreedrenodrm.la \
	$(top_builddir)/src/gallium/drivers/freedreno/libfreedreno.la \
	$(top_builddir)/src/gallium/drivers/trace/libtrace.la \
	$(top_builddir)/src/gallium/drivers/rbug/librbug.la \
	$(top_builddir)/src/gallium/drivers/noop/libnoop.la \
	$(GALLIUM_DRI_LIB_DEPS) \
	$(LIBDRM_LIBS) \
	$(FREEDRENO_LIBS)

if HAVE_MESA_LLVM
COMMON_LDFLAGS += $(LLVM_LDFLAGS)
COMMON_LIBADD += $(LLVM_LIBS)
endif

nodist_EXTRA_kgsl_dri_la_SOURCES = dummy.cpp
kgsl_dri_la_SOURCES = target-kgsl.c
kgsl_dri_la_LDFLAGS = $(COMMON_LDFLAGS)
kgsl_dri_la_LIBADD  = $(COMMON_LIBADD)

nodist_EXTRA_msm_dri_la_SOURCES = dummy.cpp
msm_dri_la_SOURCES  = target-msm.c
msm_dri_la_LDFLAGS  = $(COMMON_LDFLAGS)
msm_dri_la_LIBADD   = $(COMMON_LIBADD)

# Provide compatibility with scripts for the old Mesa build system for
# a while by putting a link to the driver into /lib of the build tree.
all-local: kgsl_dri.la msm_dri.la
	$(MKDIR_P) $(top_builddir)/$(LIB_DIR)/gallium
	ln -f .libs/kgsl_dri.so $(top_builddir)/$(LIB_DIR)/gallium/kgsl_dri.so
	ln -f .libs/msm_dri.so $(top_builddir)/$(LIB_DIR)/gallium/msm_dri.so
