ifneq ($(strip $(shell firmtool -v 2>&1 | grep usage)),)
$(error "Please install firmtool v1.1 or greater")
endif


include version.mk

# Disable kext and firmlaunch patches, all custom sysmodules except Loader, enable PASLR.
# Dangerous. Don't enable this unless you know what you're doing!
export BUILD_FOR_EXPLOIT_DEV ?= 0

# Build with O0 & frame pointer information for use with GDB
export BUILD_FOR_GDB ?= 0

# Default 3DSX TitleID for hb:ldr
export HBLDR_DEFAULT_3DSX_TID ?= 000400000D921E00

# What to call the title corresponding to HBLDR_DEFAULT_3DSX_TID
export HBLDR_DEFAULT_3DSX_TITLE_NAME ?= "hblauncher_loader"

NAME		:=	$(notdir $(CURDIR))
REVISION	:=	$(shell git describe --tags --match v[0-9]* --abbrev=8 | sed 's/-[0-9]*-g/-/')

ifeq ($(strip $(NEXUS_VERSION_BUILD)),0)
NEXUS_VERSION := v$(NEXUS_VERSION_MAJOR).$(NEXUS_VERSION_MINOR)
else
NEXUS_VERSION := v$(NEXUS_VERSION_MAJOR).$(NEXUS_VERSION_MINOR).$(NEXUS_VERSION_BUILD)
endif

BASE_SUBFOLDERS	:=	sysmodules arm11 arm9
SUBFOLDERS	:=	$(BASE_SUBFOLDERS) sysplugin k11_extension

.PHONY:	all release clean $(SUBFOLDERS) sysplugin-tools

all:		boot.firm

release:	$(NAME)$(NEXUS_VERSION).zip

clean:
	@$(foreach dir, $(SUBFOLDERS), $(MAKE) -C $(dir) clean &&) true
	@rm -rf *.firm *.zip *.3dsx *.3nr

# boot.3dsx comes from https://github.com/fincs/new-hbmenu/releases
$(NAME)$(NEXUS_VERSION).zip:	hbmenu.zip boot.firm boot.3nr
	@cp $< $@
	@zip $@ boot.firm boot.3nr -x "*.DS_Store*" "*__MACOSX*"

boot.firm:	$(BASE_SUBFOLDERS) boot.3nr k11_extension
	@firmtool build $@ -D sysmodules/sysmodules.bin arm11/arm11.elf arm9/arm9.elf k11_extension/k11_extension.elf \
	-A 0x18180000 -C XDMA XDMA NDMA XDMA
	@echo built... $(notdir $@)

hbmenu.zip:
	@curl -sSfL $(shell curl -s https://api.github.com/repos/devkitPro/3ds-hbmenu/releases/latest | grep 'browser_' | cut -d\" -f4) -o $@
	@echo downloaded... $(notdir $@)

sysplugin-tools:
	@$(MAKE) -C sysplugin tools

sysmodules: sysplugin-tools

$(BASE_SUBFOLDERS):
	@$(MAKE) -C $@ all

sysplugin: sysmodules
	@$(MAKE) -C $@ all

boot.3nr: sysplugin


k11_extension: boot.3nr
	@$(MAKE) -C $@ all