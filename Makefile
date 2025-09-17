
# include support for formulas
ifndef FORMULASUPPORT
FORMULASUPPORT = 1
endif

# exclude MQTT support
ifndef DISABLE_MQTT
DISABLE_MQTT = 0
endif

# exclude SSL for curl (has no effect if libcurl is dynamically linked)
ifndef CURLSSL_DISABLE
DISABLE_CURLSSL = 0
endif

# specify what libraries should be static linked (e.g. mparser is not available on RedHat 8)

# muParser, formular parser, available on fedora but not on RedHat 8
# when set to 1 make will download and compile muparser
ifndef MUPARSERSTATIC
MUPARSERSTATIC = 1
endif

# libmodbus, not available e.g. on cerbos GX
ifndef MODBUSSTATIC
MODBUSSTATIC   = 0
endif

# paho (mqtt) static or dynamic
ifndef PAHOSTATIC
PAHOSTATIC     = 0
endif

# libcurl static or dynamic, currently (08/2023) raspberry as well as Fedora 38
# have versions installed that does not support websockets
ifndef CURLSTATIC
CURLSTATIC = 1
endif


TARGETS = emModbus2influx

# OS dependend executables
ifndef j
j = 8
endif
MAKE           = make -j$(j)
WGET           = wget
TAR            = tar
MAKEDIR        = mkdir -p
RM             = rm -f
RMRF           = rm -rf
COPY           = cp
ifndef $(ARCH)
#ARCH           = $(shell uname -m && mkdir -p obj-`uname -m`/influxdb-post obj-`uname -m`/ccronexpr)
ARCH           = $(shell uname -m)
endif
HOSTARCH       = $(shell uname -m)
SUDO           = sudo
INSTALLDIR     = /usr/local
INSTALLDIR_BIN = $(INSTALLDIR)/bin
INSTALLDIR_CFG = $(INSTALLDIR)/etc
INSTALLDIR_SYS = $(INSTALLDIR)/lib/systemd/system
SYSTEMD_RELOAD = systemctl daemon-reload
BZIP2          = bzip2 -d -c
XZUNPACK       = xz -d -c
DOWNLOADDIR    = $(abspath download)

ifneq ($(VERBOSE),1)
SILENT         = >/dev/null
SILENTCMAKE    = --log-level=ERROR
endif

CPPFLAGS = -fPIE -O3 -Wall -DMODBUS -Iccronexpr -DSML_NO_UUID_LIB -Wno-format-overflow

ifndef DISABLEDEBUG
ifndef NODEBUG
CPPFLAGS += -g
endif
endif

# auto generate dependency files
CPPFLAGS += -MMD

# leak finder
#CC=clang
#CXX=clang++
#CPPFLAGS += -fsanitize=address
#LIBS += -fsanitize=address -g
#-static-libasan

ifdef CONFIGURE_FLAGS
CONFIGUREHOST = $(CONFIGURE_FLAGS)
else
ifdef CROSS_COMPILE
CONFIGUREHOST = --host=$(CROSS_COMPILE) --build=$(shell gcc -dumpmachine)
endif
endif

.PHONY: default all clean info Debug cleanDebug

OBJDIR       = obj-$(ARCH)
ALLTARGETS = $(addprefix $(OBJDIR)/,$(TARGETS))

default: $(ALLTARGETS)
all: default
Debug: all
cleanDebug: clean

LIBS += -lm -lpthread

SOURCES      = $(wildcard *.c influxdb-post/*.c *.cpp ccronexpr/ccronexpr.c)
OBJECTS      = $(filter %.o, $(patsubst %.c, $(OBJDIR)/%.o, $(SOURCES)) $(patsubst %.cpp, $(OBJDIR)/%.o, $(SOURCES)))
MAINOBJS     = $(patsubst %, $(OBJDIR)/%.o,$(TARGETS))
LINKOBJECTS  = $(filter-out $(MAINOBJS), $(OBJECTS))
#MAINOBJS    = $(patsubst %, $(OBJDIR)/%.o,$(ALLTARGETS))
DEPS         = $(OBJECTS:.o=.d)

#create obj dirs
X=$(shell $(MAKEDIR) obj-$(ARCH)/influxdb-post obj-$(ARCH)/ccronexpr)

ifeq ($(DISABLE_MQTT),1)
CPPFLAGS += -DDISABLE_MQTT
else
ifeq ($(PAHOSTATIC),1)
#MQTTLIBDIR  = $(shell ./getmqttlibdir)
MQTTINSTALLDIR=mqtt-$(ARCH)
MQTTLIBDIR   = $(MQTTINSTALLDIR)/lib
MQTTLIB      = libpaho-mqtt3c.a
MQTTLIBP     = $(MQTTLIBDIR)/$(MQTTLIB)
MQTTBASEDIR  = paho
MQTTSRCDIR   = $(DOWNLOADDIR)/paho.mqtt.c
MQTTSRCFILE  = $(MQTTSRCDIR)/version.minor
MQTTBUILDDIR = paho.mqtt.build
MQTTGITSRC   = https://github.com/eclipse/paho.mqtt.c.git
LIBS        += -L./$(MQTTLIBDIR) -L./$(MQTTLIBDIR)64 -l:$(MQTTLIB)
CPPFLAGS    += -I$(MQTTINSTALLDIR)/include -DPAHO_STATIC
else
LIBS         += -lpaho-mqtt3c
endif
endif

ifeq ($(MODBUSSTATIC),1)
MODBUSVERSION  = 3.1.10
MODBUSSRCFILE  = libmodbus-$(MODBUSVERSION).tar.gz
MODBUSSRC      = https://github.com/stephane/libmodbus/releases/download/v$(MODBUSVERSION)/$(MODBUSSRCFILE)
MODBUSDIR      = modbus-$(ARCH)
MODBUSTAR      = $(DOWNLOADDIR)/$(MODBUSSRCFILE)
MODBUSMAKEDIR  = $(MODBUSDIR)/libmodbus-$(MODBUSVERSION)
MODBUSMAKE     = $(MODBUSMAKEDIR)/Makefile
LIBS          += $(MODBUSLIB)
CPPFLAGS      += -I$(MODBUSMAKEDIR)/src -DMODBUS_STATIC
MODBUSLIB      = $(OBJDIR)/libmodbus.a
else
LIBS          += `pkg-config --libs libmodbus`
CPPFLAGS      += `pkg-config --cflags libmodbus`
endif

ifeq ($(FORMULASUPPORT),1)
LIBS          += -lreadline
ifeq ($(MUPARSERSTATIC),1)
MUPARSERVERSION= 2.3.5
MUPARSERSRCFILE= v$(MUPARSERVERSION).tar.gz
MUPARSERSRC    = https://github.com/beltoforion/muparser/archive/refs/tags/$(MUPARSERSRCFILE)
MUPARSERDIR    = muparser-$(ARCH)
MUPARSERTAR    = $(DOWNLOADDIR)/$(MUPARSERSRCFILE)
MUPARSERMAKEDIR= $(MUPARSERDIR)/muparser-$(MUPARSERVERSION)
MUPARSERMAKE   = $(MUPARSERMAKEDIR)/Makefile
MUPARSERLIB    = $(MUPARSERMAKEDIR)/libmuparser.a
LIBS          += $(MUPARSERLIB)
CPPFLAGS      += -I$(MUPARSERMAKEDIR)/include -DMUPARSER_STATIC
else
LIBS          += -lmuparser
endif
else
CPPFLAGS += -DDISABLE_FORMULAS
endif

ifeq ($(CURLSTATIC),1)
CURLVERSION2 = $(subst .,_,$(CURLVERSION))
CURLVERSION  = 8.16.0
CURLSRCFILE  = curl-$(CURLVERSION).tar.xz
CURLSRC      = https://github.com/curl/curl/releases/download/curl-$(CURLVERSION2)/$(CURLSRCFILE)
CURLDIR      = curl-$(ARCH)
CURLTAR      = $(DOWNLOADDIR)/$(CURLSRCFILE)
CURLMAKEDIR  = $(CURLDIR)/curl-$(CURLVERSION)
CURLMAKE     = $(CURLMAKEDIR)/Makefile
CURLLIB      = $(CURLMAKEDIR)/lib/.libs/libcurl.a
LIBS         += $(CURLLIB)
CPPFLAGS     += -I$(CURLMAKEDIR)/include -DCURL_STATIC
CURLCONFIG   =  $(CONFIGUREHOST) --disable-file --disable-ldap --disable-ldaps --disable-tftp --disable-dict --without-libidn2 --enable-websockets --disable-ftp --disable-rtsp --disable-telnet --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-manual --disable-ntlm --disable-unix-sockets --disable-cookies --without-brotli --disable-docs --without-libpsl --without-nghttp2 --without-nghttp3
ifneq ($(DISABLE_CURLSSL),1)
CURLCONFIG   += --with-openssl
ifndef SSL_DYNLIBS
SSL_DYNLIBS = -lz -lssl -lcrypto -lzstd
endif
LIBS         += $(SSL_DYNLIBS) 
else
CURLCONFIG   += --without-ssl
endif
else
LIBS         += -lcurl
endif

$(OBJDIR):
	@$(MAKEDIR) $(DOWNLOADDIR) obj-$(ARCH)/influxdb-post obj-$(ARCH)/ccronexpr

# include dependencies if they exist
-include $(DEPS)


# ------------------------ muparser static ------------------------------------
ifeq ($(MUPARSERSTATIC),1)

$(MUPARSERTAR):
	@$(MAKEDIR) $(MUPARSERDIR)
	@echo "Downloading muparser ($(MUPARSERSRC))"
	@cd $(DOWNLOADDIR); $(WGET) $(MUPARSERSRC)

$(MUPARSERMAKE):	$(MUPARSERTAR)
	@echo "unpacking muparser ($(MUPARSERSRCFILE))"
	@$(MAKEDIR) $(MUPARSERDIR)
	@cd $(MUPARSERDIR) && $(TAR) x --gunzip < $(MUPARSERTAR);
	@echo "Generating muparser Makefile"
	@cd $(MUPARSERMAKEDIR) && cmake . -DENABLE_SAMPLES=OFF -DENABLE_OPENMP=OFF -DENABLE_WIDE_CHAR=OFF -DBUILD_SHARED_LIBS=OFF
	@echo

$(MUPARSERLIB):	$(MUPARSERMAKE)
	@echo "Compiling nuparser"
	@$(MAKE) -s -C $(MUPARSERMAKEDIR) muparser $(SILENT)
	@echo "Done compiling nuparser"
	@echo "----------------------------------------------"
endif


# ------------------------ libmodbus static ----------------------------------

ifeq ($(MODBUSSTATIC),1)

$(MODBUSTAR): | $(OBJDIR)
	@$(MAKEDIR) $(MODBUSDIR)
	@echo "Downloading $(MODBUSSRC)"
	@cd $(DOWNLOADDIR) && $(WGET) $(MODBUSSRC)


$(MODBUSMAKE):        $(MODBUSTAR)
	@echo "unpacking $(MODBUSTAR)"
	@$(MAKEDIR) $(MODBUSDIR)
	@cd $(MODBUSDIR) && gunzip -c $(MODBUSTAR) | $(TAR) x;
	

$(MODBUSLIB): $(MODBUSMAKE) | $(OBJDIR)
	@echo "Generating Makefile"
	@cd $(MODBUSMAKEDIR) && ./configure -q $(CONFIGUREHOST)
	@echo
	@echo "Compiling modbus"
	@$(MAKE) -s -C $(MODBUSMAKEDIR) $(SILENT)
	@echo "Generating $(MODBUSLIB)"
	@$(AR) r $(MODBUSLIB) $(MODBUSMAKEDIR)/src/.libs/*.o
	@$(MAKE) -s -C $(MODBUSMAKEDIR) clean $(SILENT)
	@echo "Done compiling libmodbus"
	@echo "----------------------------------------------"

endif

# ------------------------ MQTT static -----------------------------------

ifeq ($(PAHOSTATIC),1)


$(MQTTSRCFILE): | $(OBJDIR)
	@echo "Downloading $(MQTTGITSRC)"
	@if [ -d $(MQTTSRCDIR) ]; then $(RMRF) $(MQTTSRCDIR); fi
	@cd $(DOWNLOADDIR) && git clone $(MQTTGITSRC) $(SILENT)


$(MQTTLIBP): $(MQTTSRCFILE)
	@echo "Generating Makefile for paho.mqtt.c"
	@$(MAKEDIR) $(MQTTINSTALLDIR)
	@$(MAKEDIR) $(MQTTBASEDIR)/$(MQTTBUILDDIR);
	@cd  $(MQTTBASEDIR)/$(MQTTBUILDDIR) && $(RMRF) * && cmake -DCMAKE_INSTALL_PREFIX:PATH=../../$(MQTTINSTALLDIR) $(SILENTCMAKE) -DPAHO_WITH_SSL=FALSE -DPAHO_BUILD_SHARED=FALSE -DPAHO_BUILD_STATIC=TRUE -DPAHO_BUILD_DOCUMENTATION=FALSE -DPAHO_BUILD_SAMPLES=FALSE -DPAHO_ENABLE_TESTING=FALSE $(MQTTSRCDIR)
	@echo "Compiling paho.mqtt.c"
	@cd  $(MQTTBASEDIR)/$(MQTTBUILDDIR) && $(MAKE) install $(SILENT)
	@cd  $(MQTTBASEDIR)/$(MQTTBUILDDIR) && rm -f ../../$(MQTTINSTALLDIR)/lib64/*.so*
	@cd $(MQTTINSTALLDIR) && if [ ! -d lib ]; then ln -sf lib64 lib; fi
endif

# ------------------------ libmcurl static -----------------------------------
ifeq ($(CURLSTATIC),1)

$(CURLTAR):
	@$(MAKEDIR) $(CURLDIR)
	@$(MAKEDIR) $(DOWNLOADDIR)
	@echo "Downloading $(CURLSRC)"
	@cd $(DOWNLOADDIR); $(WGET) $(CURLSRC)


$(CURLMAKE):        $(CURLTAR)
	@echo "unpacking $(CURLSRCFILE)"
	@$(MAKEDIR) $(CURLDIR)
	@cd $(CURLDIR) && $(XZUNPACK) $(DOWNLOADDIR)/$(CURLSRCFILE) | $(TAR) x
	@echo "Generating Makefile" # using $(CURLCONFIG)"
	cd $(CURLMAKEDIR); ./configure $(CURLCONFIG)
	@echo

$(CURLLIB): $(CURLMAKE)
	@echo "Compiling libcurl"
	@$(MAKE) -s -C $(CURLMAKEDIR) $(SILENT)

endif


# ---------------------------------------------------------------------------

$(OBJDIR)/%.o: %.c $(MODBUSLIB) $(MQTTLIBP) $(MUPARSERLIB) $(CURLLIB) | $(OBJDIR)
	@echo -n "compiling $< to $@ "
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
	@echo ""


$(OBJDIR)/%.o: %.cpp $(MODBUSLIB) $(MQTTLIBP) $(MUPARSERLIB) $(CURLLIB) | $(OBJDIR)
	@echo -n "compiling $< to $@ "
	@$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
	@echo ""


.PRECIOUS: $(ALLTARGETS) $(ALLOBJECTS)

$(ALLTARGETS): $(OBJECTS) $(SMLLIBP) $(MQTTLIBP) $(MODBUSLIB) $(MUPARSERLIB) $(CURLLIB)
	@echo -n "linking $@ "
	@$(CXX) $@.o $(LINKOBJECTS) -Wall $(LIBS) -o $@
ifeq ($(ARCH),$(HOSTARCH))
	@$(COPY) $@ . 
endif
	@echo ""


build: clean all

mrproper: distclean
	@$(RMRF) $(DOWNLOADDIR)
	@echo "cleaned downloaded files"

install: $(ALLTARGETS)
	@echo "Installing in $(INSTALLDIR)"
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_BIN)
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_CFG)
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_SYS)
	$(SUDO) $(COPY) $(ALLTARGETS) $(INSTALLDIR_BIN)
	$(SUDO) $(COPY) emModbus2influx.service $(INSTALLDIR_SYS)
#	$(SUDO) $(COPY) emModbus2influx.conf $(INSTALLDIR_CFG)
	$(SUDO) $(SYSTEMD_RELOAD)

clean:
	@$(RMRF) $(OBJDIR)
ifeq ($(PAHOSTATIC),1)
	@$(RMRF) $(MQTTINSTALLDIR)
endif
	@echo "cleaned"

distclean:	clean
ifeq ($(FORMULASUPPORT),1)
ifeq ($(MUPARSERSTATIC),1)
	@$(RMRF) $(MUPARSERDIR)
endif
endif
ifeq ($(MODBUSSTATIC),1)
	@$(RMRF) $(MODBUSDIR)
endif
ifeq ($(CURLSTATIC),1)
	@$(RMRF) $(CURLDIR)
endif
ifeq ($(DISABLE_MQTT),0)
ifeq ($(PAHOSTATIC),1)
	@$(RMRF) $(MQTTBASEDIR)/$(MQTTBUILDDIR)
endif
endif
	@rm -rf $(OBJDIR)
	@echo "cleaned static build dirs"

info:
	@echo "          ARCH: $(ARCH)"
	@echo "      HOSTARCH: $(HOSTARCH)"
	@echo "       TARGETS: $(TARGETS)"
	@echo "    ALLTARGETS: $(ALLTARGETS)"
	@echo "       SOURCES: $(SOURCES)"
	@echo "       OBJECTS: $(OBJECTS)"
	@echo "   LINKOBJECTS: $(LINKOBJECTS)"
	@echo "      MAINOBJS: $(MAINOBJS)"
	@echo "          DEPS: $(DEPS)"
	@echo "    CC/CPP/CXX: $(CC)/$(CPP)/$(CXX)"
	@echo "        CFLAGS: $(CFLAGS)"
	@echo "      CPPFLAGS: $(CPPFLAGS)"
	@echo "      CXXFLAGS: $(CXXFLAGS)"
	@echo "          LIBS: $(LIBS)"
	@echo "    MQTTLIBDIR: $(MQTTLIBDIR)"
	@echo "       MQTTLIB: $(MQTTLIB)"
ifeq ($(PAHOSTATIC),1)
	@echo "  MODBUSSTATIC: $(MODBUSSTATIC)"
	@echo "   MQTTBASEDIR: $(MQTTBASEDIR)"
	@echo "    MQTTSRCDIR: $(MQTTSRCDIR)"
	@echo "  MQTTBUILDDIR: $(MQTTBUILDDIR)"
endif
ifeq ($(MODBUSSTATIC),1)
	@echo "     MODBUSLIB: $(MODBUSLIB)"
	@echo "     MODBUSDIR: $(MODBUSDIR)"
	@echo "     MODBUSTAR: $(MODBUSTAR)"
endif
	@echo "    CURLSTATIC: $(CURLSTATIC)"
ifeq ($(CURLSTATIC),1)
	@echo "   CURLVERSION: $(CURLVERSION) ($(CURLVERSION2))"
	@echo "       CURLLIB: $(CURLLIB)"
	@echo "       CURLDIR: $(CURLDIR)"
	@echo "       CURLTAR: $(CURLTAR)"
	@echo "       CURLSRC: $(CURLSRC)"
	@echo "    CURLCONFIG: $(CURLCONFIG)"
	@echo "   SSL_DYNLIBS: $(SSL_DYNLIBS)"
endif
	@echo "MUPARSERSTATIC: $(MUPARSERSTATIC)"
ifeq ($(MUPARSERSTATIC),1)
	@echo "   MUPARSERLIB: $(MUPARSERLIB)"
	@echo "   MUPARSERTAR: $(MUPARSERTAR)"
	@echo "   MUPARSERDIR: $(MUPARSERDIR)"
endif
	@echo "   INSTALLDIRS: $(INSTALLDIR_BIN) $(INSTALLDIR_CFG) $(INSTALLDIR_SYS)"
#	@echo "$(notdir $(MUPARSERSRC))"
	@echo " CROSS_COMPILE: $(CROSS_COMPILE) ($(CONFIGUREHOST))"
	@echo "  DISABLE_MQTT: $(DISABLE_MQTT)"
	@echo "FORMULASUPPORT: $(FORMULASUPPORT)"
	@echo "   DOWNLOADDIR: $(DOWNLOADDIR)"
