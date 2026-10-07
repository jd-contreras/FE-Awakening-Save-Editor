#---------------------------------------------------------------------------------
# FE:A Save Editor - 3DS homebrew (.3dsx), devkitARM + libctru + citro2d
#
# Based on the devkitPro 3ds application template.
# NOTE: GNU make cannot handle spaces in paths. Keep this project in a folder
#       whose full path has no spaces (e.g. G:/dev/fea-save-editor).
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files (embedded via bin2o)
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS (unused for now)
#---------------------------------------------------------------------------------
TARGET		:=	FEAEditor
BUILD		:=	build
SOURCES		:=	source source/core
DATA		:=	data
INCLUDES	:=	include
#ROMFS		:=	romfs

APP_TITLE		:=	Awakening Save Editor
APP_DESCRIPTION	:=	Fire Emblem Awakening save editor
APP_AUTHOR		:=	Noble Zero
APP_VERSION		:=	0.1.0

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -Wextra -O2 -mword-relocations \
			-ffunction-sections -std=gnu11 \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -DAPP_VERSION=\"$(APP_VERSION)\"

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_SOURCES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES		:=	$(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

# Icon: icon.png in the project root if present, else libctru's default icon
ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring icon.png,$(icons)))
		export APP_ICON := $(TOPDIR)/icon.png
	else
		export APP_ICON := $(CTRULIB)/default_icon.png
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean cia

#---------------------------------------------------------------------------------
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia

#---------------------------------------------------------------------------------
# .cia build: make cia  (tools/cia: makerom.exe, bannertool.exe, cia.rsf)
# 3D banner: banner_3d/ with common.cgfx (or .bcres), optional region10-13 and banner.bcwav
# (tools/cia/pack_cbmd.py). Otherwise a flat banner from banner.png (256x128) and banner.wav,
# made from icon.png if missing.
#---------------------------------------------------------------------------------
CIA_TOOLS := tools/cia
PYTHON    ?= python  # override if python is not on the devkitPro PATH
cia: all
	@if [ -f banner_3d/common.cgfx ] || [ -f banner_3d/common.bcres ]; then \
		$(PYTHON) $(CIA_TOOLS)/pack_cbmd.py banner_3d $(BUILD)/banner.bnr; \
	else \
		if [ ! -f banner.png ] || [ ! -f banner.wav ]; then $(PYTHON) $(CIA_TOOLS)/make_banner.py; fi; \
		$(CIA_TOOLS)/bannertool.exe makebanner -i banner.png -a banner.wav -o $(BUILD)/banner.bnr; \
	fi
	@$(CIA_TOOLS)/makerom.exe -f cia -o $(TARGET).cia -elf $(TARGET).elf -rsf $(CIA_TOOLS)/cia.rsf 		-icon $(TARGET).smdh -banner $(BUILD)/banner.bnr -exefslogo -target t -ver 0
	@echo built ... $(TARGET).cia

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# binary data embedded with bin2o
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
