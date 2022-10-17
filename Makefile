# specify what libraries should be static linked (e.g. mparser is not available on RedHat 8)

FORMULASUPPORT = 1
# muParser, formular parser, available on fedora but not on RedHat 8
# when set to 1 make will download and compile muparser
MUPARSERSTATIC = 0

# libmodbus, not available e.g. on cerbos GX
MODBUSSTATIC   = 0

# paho (mqtt) static or dynamic
PAHOSTATIC     = 0


TARGETS = emModbus2influx

# OS dependend executables
WGET           = wget -q --show-progress
TAR            = tar
MAKEDIR        = mkdir -p
RM             = rm -f
RMRF           = rm -rf
COPY           = cp
ARCH           = $(shell uname -m && mkdir -p obj-`uname -m`/influxdb-post)
SUDO           = sudo
INSTALLDIR     = /usr/local
INSTALLDIR_BIN = $(INSTALLDIR)/bin
INSTALLDIR_CFG = $(INSTALLDIR)/etc
INSTALLDIR_SYS = $(INSTALLDIR)/lib/systemd/system
SYSTEMD_RELOAD = systemctl daemon-reload

ALLTARGETS = $(TARGETS:=$(TGT))

CPPFLAGS = -fPIE -g0 -Os -Wall -g -Imqtt$(TGT)/include -DSML_NO_UUID_LIB

# auto generate dependency files
CPPFLAGS += -MMD

.PHONY: default all clean info Debug cleanDebug

default: $(ALLTARGETS)
all: default
Debug: all
cleanDebug: clean

LIBS = -lm -lpthread
OBJDIR       = obj-$(ARCH)$(TGT)
SOURCES      = $(wildcard *.c influxdb-post/*.c *.cpp)
OBJECTS      = $(filter %.o, $(patsubst %.c, $(OBJDIR)/%.o, $(SOURCES)) $(patsubst %.cpp, $(OBJDIR)/%.o, $(SOURCES)))
MAINOBJS     = $(patsubst %, $(OBJDIR)/%.o,$(TARGETS))
LINKOBJECTS  = $(filter-out $(MAINOBJS), $(OBJECTS))
#MAINOBJS    = $(patsubst %, $(OBJDIR)/%.o,$(ALLTARGETS))
DEPS         = $(OBJECTS:.o=.d)

ifeq ($(PAHOSTATIC),1)
#MQTTLIBDIR  = $(shell ./getmqttlibdir)
ifeq ($(TGT),-gx)
MQTTLIBDIR   = mqtt-gx/lib
else
MQTTLIBDIR   = mqtt/lib64
endif
MQTTLIB      = libpaho-mqtt3c.a
MQTTLIBP     = $(MQTTLIBDIR)/$(MQTTLIB)
LIBS       += -L./$(MQTTLIBDIR) -l:$(MQTTLIB)
else
LIBS         += -lpaho-mqtt3c
endif

ifeq ($(MODBUSSTATIC),1)
MODBUSVERSION  = 3.1.8
MODBUSSRCFILE  = libmodbus-$(MODBUSVERSION).tar.gz
MODBUSSRC      = https://github.com/stephane/libmodbus/releases/download/v$(MODBUSVERSION)/$(MODBUSSRCFILE)
MODBUSDIR      = modbus$(TGT)
MODBUSTAR      = $(MODBUSDIR)/$(MODBUSSRCFILE)
MODBUSMAKEDIR  = $(MODBUSDIR)/libmodbus-$(MODBUSVERSION)
MODBUSMAKE     = $(MODBUSMAKEDIR)/Makefile
LIBS          += $(MODBUSLIB)
CPPFLAGS      += -I$(MODBUSMAKEDIR)/src
MODBUSLIB      = $(OBJDIR)/libmodbus.a
else
LIBS          += `pkg-config --libs libmodbus`
CPPFLAGS      += `pkg-config --cflags libmodbus`
endif

ifeq ($(FORMULASUPPORT),1)
LIBS          += -lreadline
ifeq ($(MUPARSERSTATIC),1)
MUPARSERVERSION= 2.3.3-1
MUPARSERSRCFILE= v$(MUPARSERVERSION).tar.gz
MUPARSERSRC    = https://github.com/beltoforion/muparser/archive/refs/tags/$(MUPARSERSRCFILE)
MUPARSERDIR    = muparser$(TGT)
MUPARSERTAR    = $(MUPARSERDIR)/$(MUPARSERSRCFILE)
MUPARSERMAKEDIR= $(MUPARSERDIR)/muparser-$(MUPARSERVERSION)
MUPARSERMAKE   = $(MUPARSERMAKEDIR)/Makefile
MUPARSERLIB    = $(MUPARSERMAKEDIR)/libmuparser.a
LIBS          += $(MUPARSERLIB)
CPPFLAGS      += -I$(MUPARSERMAKEDIR)/include
else
LIBS          += -lmuparser
endif
endif


# include dependencies if they exist
-include $(DEPS)

# ------------------------ muparser static ------------------------------------
ifeq ($(MUPARSERSTATIC),1)

$(MUPARSERTAR):
	@$(MAKEDIR) $(MUPARSERDIR)
	@echo "Downloading $(MUPARSERSRC)"
	@cd $(MUPARSERDIR); $(WGET) $(MUPARSERSRC)

$(MUPARSERMAKE):	$(MUPARSERTAR)
	@echo "unpacking $(MUPARSERSRCFILE)"
	@cd $(MUPARSERDIR); $(TAR) x --gunzip < $(MUPARSERSRCFILE);
	@echo "Generating Makefile"
	@cd $(MUPARSERMAKEDIR); cmake . -DENABLE_SAMPLES=OFF -DENABLE_OPENMP=OFF -DENABLE_WIDE_CHAR=OFF -DBUILD_SHARED_LIBS=OFF
	@echo

$(MUPARSERLIB):	$(MUPARSERMAKE)
	@echo "Compiling nuparser"
	@$(MAKE) -j 4 -s -C $(MUPARSERMAKEDIR) muparser
endif


# ------------------------ libmodbus static ----------------------------------

ifeq ($(MODBUSSTATIC),1)

$(MODBUSTAR):
	@$(MAKEDIR) $(MODBUSDIR)
	@echo "Downloading $(MODBUSSRC)"
	@cd $(MODBUSDIR); $(WGET) $(MODBUSSRC)

$(MODBUSMAKE):        $(MODBUSTAR)
	@echo "unpacking $(MODBUSSRCFILE)"
	@cd $(MODBUSDIR); $(TAR) x --gunzip < $(MODBUSSRCFILE);
	@echo "Generating Makefile"
	@cd $(MODBUSMAKEDIR); ./configure -q
	@echo

$(MODBUSLIB): $(MODBUSMAKE)
	@echo "Compiling modbus"
	@$(MAKE) -s -C $(MODBUSMAKEDIR)
	@echo "Generating $(MODBUSLIB)"
	@$(AR) r $(MODBUSLIB) $(MODBUSMAKEDIR)/src/.libs/*.o
	@$(MAKE) -s -C $(MODBUSMAKEDIR) clean

endif


ifeq ($(PAHOSTATIC),1)
$(MQTTLIBP):
	@cd paho; ./buildmqtt || exit 1; cd ..
endif


$(OBJDIR)/%.o: %.c $(MODBUSLIB) $(MQTTLIBP) $(MUPARSERLIB)
	@echo -n "compiling $< to $@ "
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
	@echo ""


$(OBJDIR)/%.o: %.cpp $(MODBUSLIB) $(MQTTLIBP) $(MUPARSERLIB)
	@echo -n "compiling $< to $@ "
	@$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
	@echo ""


.PRECIOUS: $(TARGETS) $(ALLOBJECTS)

$(ALLTARGETS): $(OBJECTS) $(SMLLIBP) $(MQTTLIBP) $(MODBUSLIB) $(MUPARSERLIB)
	@echo -n "linking $@ "
	$(CXX) $(OBJDIR)/$(patsubst %$(TGT),%,$@).o $(LINKOBJECTS) -Wall $(LIBS) -o $@
	@echo ""


build: clean all

install: $(ALLTARGETS)
	@echo "Installing in $(INSTALLDIR)"
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_BIN)
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_CFG)
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_SYS)
	$(SUDO) $(COPY) $(TARGETS) $(INSTALLDIR_BIN)
	$(SUDO) $(COPY) emModbus2influx.service $(INSTALLDIR_SYS)
#	$(SUDO) $(COPY) emModbus2influx.conf $(INSTALLDIR_CFG)
	$(SUDO) $(SYSTEMD_RELOAD)

clean:
	@$(RM) $(OBJECTS) $(TARGETS) $(DEPS) $(MUPARSERLIB) $(MODBUSLIB) $(MQTTLIBP)
	@echo "cleaned"

distclean:	clean
ifeq ($(MUPARSERSTATIC),1)
	@$(RMRF) $(MUPARSERDIR)
endif
ifeq ($(MODBUSSTATIC),1)
	@$(RMRF) $(MODBUSDIR)
endif
	rm -rf $(OBJDIR)
	@echo "cleaned static build dirs"

info:
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
	@echo "  MODBUSSTATIC: $(MODBUSSTATIC)"
	@echo "     MODBUSLIB: $(MODBUSLIB)"
	@echo "     MODBUSDIR: $(MODBUSDIR)"
	@echo "     MODBUSTAR: $(MODBUSTAR)"
	@echo "MUPARSERSTATIC: $(MUPARSERSTATIC)"
	@echo "   MUPARSERLIB: $(MUPARSERLIB)"
	@echo "   MUPARSERTAR: $(MUPARSERTAR)"
	@echo "   INSTALLDIRS: $(INSTALLDIR_BIN) $(INSTALLDIR_CFG) $(INSTALLDIR_SYS)"
	@echo "$(notdir $(MUPARSERSRC))"
