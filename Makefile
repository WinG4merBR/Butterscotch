.SUFFIXES:

ifeq ($(strip $(PS3DEV)),)
  ifeq ($(strip $(DEVKITPS3)),)
    export PS3DEV := /usr/local/ps3dev
  else
    export PS3DEV := $(DEVKITPS3)
  endif
endif

ifeq ($(strip $(PSL1GHT)),)
$(error Please set PSL1GHT in your environment)
endif

PROJECT_ROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
export PROJECT_ROOT

export PATH             := $(PS3DEV)/bin:$(PS3DEV)/ppu/bin:$(PATH)
export PORTLIBS         := $(PS3DEV)/portlibs/ppu
export LIBPSL1GHT_INC   := -I$(PSL1GHT)/ppu/include -I$(PSL1GHT)/ppu/include/simdmath
export LIBPSL1GHT_LIB   := -L$(PSL1GHT)/ppu/lib

TARGET      := butterscotch
BUILD       := build_ps3

SOURCES 	:= \
	src/core \
	src/data \
	src/engine \
	src/fs \
	src/input \
	src/renderer \
	src/audio \
	src/platform/ps3 \
	src/platform/ps3/rsx

DATA :=
INCLUDES := \
	$(shell find $(PROJECT_ROOT)/src -type d) \
	$(PROJECT_ROOT)/ps3gl/include \
	$(PROJECT_ROOT)/vendor/stb/ds \
	$(PROJECT_ROOT)/vendor/stb/image

PKGFILES    := $(PROJECT_ROOT)/PKG_TEMPLATE
PKG_USRDIR  := $(PKGFILES)/USRDIR
DATA_WIN    ?= $(PROJECT_ROOT)/data.win

TITLE ?= Butterscotch
APPID ?= BTSC00001
MIN_VER     := 460
ATTRIBUTES  := 0x32
CONTENTID   := UP0001-$(APPID)_00-0000000000000000
ICON0       := $(PROJECT_ROOT)/PKG_TEMPLATE/ICON0.PNG
SFOXML      := $(PS3DEV)/bin/sfo.xml

CFLAGS      := -O2 -Wall -Wextra -mcpu=cell -mhard-float \
               -fmodulo-sched -ffunction-sections -fdata-sections -fno-builtin \
               -D__PPU__ -D__PS3__ -D__CELLOS_LV2__ \
			   -DPS3_DATA_WIN_PATH=\"/dev_hdd0/game/$(APPID)/USRDIR/data.win\" \
               $(MACHDEP) $(INCLUDE)

CXXFLAGS    := $(CFLAGS)

LDFLAGS     := $(MACHDEP) -mcpu=cell -mhard-float -Wl,-Map,$(notdir $@).map

PS3GL_LOCAL_LIB := $(PROJECT_ROOT)/ps3gl/libPS3GL.a
ifeq ($(wildcard $(PS3GL_LOCAL_LIB)),)
PS3GL_LINK := -lPS3GL
else
PS3GL_LINK := $(PS3GL_LOCAL_LIB)
endif

LIBS        := $(PS3GL_LINK) -lsimdmath -lrsx -lgcm_sys -lnet -lsysutil -lsysfs -lio -lm -lrt -llv2 -lc
LIBDIRS     := $(PORTLIBS)

include $(PSL1GHT)/ppu_rules

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(PROJECT_ROOT)/$(TITLE)

export VPATH    := $(foreach dir,$(SOURCES),$(PROJECT_ROOT)/$(dir)) \
                   $(foreach dir,$(DATA),$(PROJECT_ROOT)/$(dir))

export DEPSDIR  := $(PROJECT_ROOT)/$(BUILD)
export BUILDDIR := $(PROJECT_ROOT)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(PROJECT_ROOT)/$(dir)/*.c)))
CFILES      := $(filter-out ps3gl.c ffp_shader_fpo.c ffp_shader_vpo.c,$(CFILES))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(PROJECT_ROOT)/$(dir)/*.cpp)))
sFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(PROJECT_ROOT)/$(dir)/*.s)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(PROJECT_ROOT)/$(dir)/*.S)))


ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES   := $(addsuffix .o,$(BINFILES)) \
                   $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
                   $(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   $(LIBPSL1GHT_INC) \
                   -I$(PROJECT_ROOT)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   $(LIBPSL1GHT_LIB)

.PHONY: all elf self pkg clean rebuild assets check_data_win

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) -C $(BUILD) -f $(PROJECT_ROOT)/Makefile


$(PKG_USRDIR):
	@mkdir -p $@

check_data_win:
	@if [ ! -f "$(DATA_WIN)" ]; then \
		echo "Missing data.win at $(DATA_WIN). Use 'make pkg DATA_WIN=/path/to/data.win'."; \
		exit 1; \
	fi

assets: check_data_win | $(PKG_USRDIR)
	cp "$(DATA_WIN)" "$(PKG_USRDIR)/data.win"

elf: $(BUILD)
	@$(MAKE) -C $(BUILD) -f $(PROJECT_ROOT)/Makefile $(OUTPUT).elf

self: $(BUILD)
	@$(MAKE) -C $(BUILD) -f $(PROJECT_ROOT)/Makefile $(OUTPUT).self

pkg: assets $(BUILD)
	@$(MAKE) -C $(BUILD) -f $(PROJECT_ROOT)/Makefile \
		$(OUTPUT).pkg \
		TITLE="$(TITLE)" \
		APPID="$(APPID)" \
		MIN_VER="$(MIN_VER)" \
		ATTRIBUTES="$(ATTRIBUTES)" \
		CONTENTID="$(CONTENTID)" \
		ICON0="$(ICON0)" \
		SFOXML="$(SFOXML)"
clean:
	@echo clean ...
	@rm -rf $(PROJECT_ROOT)/$(BUILD) \
	        $(PROJECT_ROOT)/*.elf \
	        $(PROJECT_ROOT)/*.self \
	        $(PROJECT_ROOT)/*.fake.self \
	        $(PROJECT_ROOT)/*.pkg \
			$(PROJECT_ROOT)/*.gnpdrm.pkg \
	        $(PKG_USRDIR)/data.win

test: $(BUILD)
	@echo "===> Testing full compile pipeline (ephemeral)"
	@$(MAKE) -C $(BUILD) -f $(PROJECT_ROOT)/Makefile $(OUTPUT).elf

	@echo "Cleaning build artifacts..."
	@rm -f "$(BUILD)/$(OUTPUT).elf"
rebuild: clean all

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
$(OUTPUT).pkg: $(OUTPUT).self

-include $(DEPENDS)

endif
