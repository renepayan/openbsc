AM_CPPFLAGS = \
	$(all_includes) \
	-I$(top_srcdir)/include \
	-I$(top_builddir) \
	$(NULL)

AM_CFLAGS = \
	-Wall \
	$(LIBOSMOCORE_CFLAGS) \
	$(LIBOSMOGSM_CFLAGS) \
	$(LIBOSMOGSM_CFLAGS) \
	$(LIBOSMOVTY_CFLAGS) \
	$(COVERAGE_CFLAGS) \
	$(NULL)

AM_LDFLAGS = \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(COVERAGE_LDFLAGS) \
	$(NULL)

# Libraries
SUBDIRS = \
	libcommon \
	libmgcp \
	libbsc \
	libmsc \
	libtrau \
	libfilter \
	libcommon-cs \
	$(NULL)

# Conditional Libraries
if BUILD_IU
SUBDIRS += \
	libiu \
	$(NULL)
endif

# Programs
SUBDIRS += \
	osmo-nitb \
	osmo-bsc_mgcp \
	utils \
	$(NULL)

# Conditional Programs
if BUILD_NAT
SUBDIRS += \
	osmo-bsc_nat \
	$(NULL)
endif

if BUILD_BSC
SUBDIRS += \
	osmo-bsc \
	$(NULL)
endif
