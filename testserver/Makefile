TARGETS = testserver

# OS dependend executables
MAKE           = make -j4
WGET           = wget
TAR            = tar
MAKEDIR        = mkdir -p
RM             = rm -f
RMRF           = rm -rf
COPY           = cp

ifndef $(ARCH)
ARCH         = $(shell uname -m)
endif
HOSTARCH       = $(shell uname -m)

OBJDIR       = obj-$(ARCH)

ALLTARGETS = $(addprefix $(OBJDIR)/,$(TARGETS))

#CC = gcc
CFLAGS = -g0 -Os -Wall -g

# auto generate dependency files
CFLAGS += -MMD

ifdef CONFIGURE_FLAGS
CONFIGUREHOST = $(CONFIGURE_FLAGS)
else
ifdef CROSS_COMPILE
CONFIGUREHOST = --host=$(CROSS_COMPILE) --build=$(shell gcc -dumpmachine)
endif
endif


.PHONY: default all clean info Debug cleanDebug


# libmodbus, not available e.g. on cerbos GX
MODBUSSTATIC   = 1

ifeq ($(MODBUSSTATIC),1)
MODBUSVERSION  = 3.1.11
MODBUSSRCFILE  = libmodbus-$(MODBUSVERSION).tar.gz
MODBUSSRC      = https://github.com/stephane/libmodbus/releases/download/v$(MODBUSVERSION)/$(MODBUSSRCFILE)
MODBUSDIR      = modbus$(TGT)
MODBUSTAR      = $(MODBUSDIR)/$(MODBUSSRCFILE)
MODBUSMAKEDIR  = $(MODBUSDIR)/libmodbus-$(MODBUSVERSION)
MODBUSMAKE     = $(MODBUSMAKEDIR)/Makefile
LIBS          += $(MODBUSLIB)
CPPFLAGS      += -I$(MODBUSMAKEDIR)/src -DMODBUS_STATIC
MODBUSLIB      = $(OBJDIR)/libmodbus.a
else
LIBS          += `pkg-config --libs libmodbus`
CPPFLAGS      += `pkg-config --cflags libmodbus`
endif


default: $(ALLTARGETS)


all: default
Debug: all
cleanDebug: clean

LIBS += -lstdc++
SOURCES      = $(wildcard *.c *.cpp)
#OBJECTS     = $(patsubst %.c, %.o, $(SOURCES))
OBJECTS      = $(filter %.o, $(patsubst %.c, $(OBJDIR)/%.o, $(SOURCES)) $(patsubst %.cpp, $(OBJDIR)/%.o, $(SOURCES)))
MAINOBJS     = $(patsubst %, $(OBJDIR)/%.o,$(TARGETS))
LINKOBJECTS = $(filter-out $(MAINOBJS), $(OBJECTS))
DEPS         = $(OBJECTS:.o=.d)


$(OBJDIR):
		@$(MAKEDIR) $(OBJDIR)


# include dependencies if they exist
-include $(DEPS)

# ------------------------ libmodbus static ----------------------------------

ifeq ($(MODBUSSTATIC),1)

$(MODBUSTAR):
	@$(MAKEDIR) $(MODBUSDIR)
	@echo "Downloading $(MODBUSSRC)"
	@cd $(MODBUSDIR); $(WGET) $(MODBUSSRC)


$(MODBUSMAKE):        $(MODBUSTAR)
	@echo "unpacking $(MODBUSSRCFILE)"
	@cd $(MODBUSDIR); $(TAR) x --gunzip < $(MODBUSSRCFILE);

$(MODBUSLIB): $(MODBUSMAKE) | $(OBJDIR)
	@echo "Generating libmodbus Makefile"
	@cd $(MODBUSMAKEDIR); ./configure -q $(CONFIGUREHOST)
	@echo
	@echo "Compiling modbus"
	@$(MAKE) -s -C $(MODBUSMAKEDIR)
	@echo "Generating $(MODBUSLIB)"
	$(AR) r $(MODBUSLIB) $(MODBUSMAKEDIR)/src/.libs/*.o
	@$(MAKE) -s -C $(MODBUSMAKEDIR) clean
endif


$(OBJDIR)/%.o: %.c $(MODBUSLIB) | $(OBJDIR)
	@echo -n "compiling $< to $@ "
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -c $< -o $@
	@echo ""

$(OBJDIR)/%.o: %.cpp $(MODBUSLIB) | $(OBJDIR)
	@echo -n "compiling $< to $@ "
	@mkdir -p $(@D)
	@$(CXX) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
	@echo ""


.PRECIOUS: $(ALLTARGETS) $(ALLOBJECTS)

$(ALLTARGETS): $(OBJECTS)
	@echo -n "linking $@"
	@$(CXX) $@.o $(LINKOBJECTS) -Wall $(LIBS) -o $@
	@echo ""
ifeq ($(ARCH),$(HOSTARCH))
	@$(COPY) $@ .
endif


build: clean all

clean:
	@$(RM) $(OBJECTS)
	@$(RM) $(TARGETS)
	@$(RM) $(DEPS)
	@$(RM) jk-flddefs.h mkflddefs $(OBJDIR)/mkflddefs
	@$(RMRF) $(OBJDIR)
	@$(MAKE) ARCH=$(ARCH) TGT=$(TGT) -C $(JK-API-DIR) clean

info:
	@echo "         ARCH: $(ARCH)"
	@echo "     HOSTARCH: $(HOSTARCH)"
	@echo "          TGT: $(TGT)"
	@echo "      TARGETS: $(TARGETS)"
	@echo "      SOURCES: $(SOURCES)"
	@echo "      OBJECTS: $(OBJECTS)"
	@echo "  LINKOBJECTS: $(LINKOBJECTS)"
	@echo "     MAINOBJS: $(MAINOBJS)"
	@echo "         DEPS: $(DEPS)"
	@echo "           CC: $(CC)"
	@echo "          CXX: $(CXX)"
	@echo "       CFLAGS: $(CFLAGS)"
	@echo "         LIBS: $(LIBS)"
	@echo " MODBUSSTATIC: $(MODBUSSTATIC)"
ifeq ($(MODBUSSTATIC),1)
	@echo "    MODBUSLIB: $(MODBUSLIB)"
	@echo "    MODBUSDIR: $(MODBUSDIR)"
	@echo "    MODBUSTAR: $(MODBUSTAR)"
	@echo "MODBUSMAKEDIR: $(MODBUSMAKEDIR)"
endif

