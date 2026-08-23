#---------------------------------------------------------------------------------
.SUFFIXES:

.DEFAULT_GOAL := all
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

# Default 3DSX TitleID for hb:ldr (note: also defined in top-level Makefile)
export HBLDR_DEFAULT_3DSX_TID ?= 000400000D921E00

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

ifeq ($(BUILD_FOR_EXPLOIT_DEV),1)
	DEFINES :=	-D__3DS__ -DHBLDR_DEFAULT_3DSX_TID="0x$(HBLDR_DEFAULT_3DSX_TID)ULL" -DBUILD_FOR_EXPLOIT_DEV=1
else
	DEFINES :=	-D__3DS__ -DHBLDR_DEFAULT_3DSX_TID="0x$(HBLDR_DEFAULT_3DSX_TID)ULL"
endif

#BUILD_FOR_GDB  := 1

ifeq ($(BUILD_FOR_GDB),1)
	OPTFLAGS := -O0
	LIBS := -lctrud
else
	OPTFLAGS := -O2 -fomit-frame-pointer
	LIBS := -lctru
endif

COMMON_FLAGS =\
		-g \
		-Wall \
		-Wextra \
		-Werror \
		-mword-relocations \
		-ffunction-sections \
		-fdata-sections \
		$(OPTFLAGS) \
		$(ARCH) \
		$(DEFINES) \
		$(INCLUDE)

CFLAGS		:= -std=gnu11 $(COMMON_FLAGS)

CXXFLAGS	:= -fno-rtti -fno-exceptions -std=gnu++17 $(COMMON_FLAGS)

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map),-wrap,exit,--section-start,.text=0x14000000

# Sysplugin support
ifneq ($(wildcard $(TOPDIR)/3dsx.specs),)
LDFLAGS = -specs=$(TOPDIR)/3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map),-wrap,exit,--section-start,.text=0x14000000
ifneq ($(wildcard $(TOPDIR)/plgmarkers.ld),)
LDFLAGS += -Wl,-T,$(TOPDIR)/plgmarkers.ld
endif
endif

# Semantic sysplugin hook identities
SEMANTIC_MARKER_PLUGIN := $(abspath $(TOPDIR)/../../sysplugin/build/semantic_marker_plugin.so)
SEMANTIC_MARKER_DIR := $(abspath $(TOPDIR)/build/semantic_markers)
SEMANTIC_MARKER_FLAGS := -fplugin=$(SEMANTIC_MARKER_PLUGIN) -fplugin-arg-semantic_marker_plugin-out-dir=$(SEMANTIC_MARKER_DIR)
CFLAGS += $(SEMANTIC_MARKER_FLAGS)
CXXFLAGS += $(SEMANTIC_MARKER_FLAGS)

$(SEMANTIC_MARKER_PLUGIN):
	@$(MAKE) -C $(TOPDIR)/../../sysplugin tools

LIBS	:=	-lctru

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(CTRULIB)


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
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 		:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# Sysplugin support
.PHONY: $(BUILD) clean all elf semantic-marker-dir

#---------------------------------------------------------------------------------
all: semantic-marker-dir $(BUILD)

semantic-marker-dir:
	@mkdir -p $(BUILD)/semantic_markers

# Sysplugin support
elf: semantic-marker-dir
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $(CURDIR)/$(TARGET).elf

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# Sysplugin support
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).cxi $(TARGET).elf


#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).cxi

$(OUTPUT).cxi	:	$(OUTPUT).elf $(OUTPUT).rsf
	@makerom -f ncch -rsf $(word 2,$^) -nocodepadding -o $@ -elf $<
	@echo built ... $(notdir $@)

$(OUTPUT).elf	:	$(OFILES)

memory.o	:	CFLAGS += -O3

%.elf: $(OFILES)
	@echo linking $(notdir $@)
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@
	@$(NM) -CSn $@ > $(notdir $*.lst)

$(OFILES_SRC)	: $(HFILES_BIN) $(SEMANTIC_MARKER_PLUGIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
