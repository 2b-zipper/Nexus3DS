.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TARGET := fixer
BUILD := build
SOURCES := source
INCLUDES := include
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mgeneral-regs-only
CFLAGS := -g -std=gnu11 -Wall -Wextra -Werror -Os -mword-relocations -fPIE \
	-fomit-frame-pointer -ffunction-sections -fdata-sections -fno-builtin \
	-fno-unwind-tables -fno-asynchronous-unwind-tables $(ARCH) \
	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir))
LDFLAGS := $(ARCH) -nostdlib -nostartfiles -Wl,-T,$(CURDIR)/3nr.ld,--gc-sections,--build-id=none,-Map,$(BUILD)/$(TARGET).map
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
NM := $(DEVKITARM)/bin/arm-none-eabi-nm
HOST_CXX ?= g++
GCC_PLUGIN_INCLUDE := $(shell $(CC) -print-file-name=plugin)/include
SEMANTIC_PLUGIN_SRC := semantic_marker_plugin.cc
SEMANTIC_PLUGIN := $(BUILD)/semantic_marker_plugin.so

CFILES := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))
OFILES := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(CFILES)))
DEPS := $(OFILES:.o=.d)
ELF := $(BUILD)/$(TARGET).elf
OUTPUT := ../boot.3nr
K11_HEADER := ../k11_extension/include/svc/SysPlugin3nrGenerated.h

.PHONY: all clean tools

all: $(SEMANTIC_PLUGIN) $(OUTPUT)

tools: $(SEMANTIC_PLUGIN)

$(BUILD):
	@mkdir -p $@

$(SEMANTIC_PLUGIN): $(SEMANTIC_PLUGIN_SRC) gmp.h | $(BUILD)
	@echo compiling $(notdir $@)
	@$(HOST_CXX) -std=gnu++17 -shared -fPIC -fno-rtti -O2 -I$(CURDIR) -I$(GCC_PLUGIN_INCLUDE) $< -o $@

$(BUILD)/%.o: source/%.c | $(BUILD)
	@echo compiling $(notdir $<)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(ELF): $(OFILES) 3nr.ld
	@echo linking $(notdir $@)
	@$(CC) $(LDFLAGS) $(OFILES) -lgcc -o $@
	@$(NM) -CSn $@ > $(BUILD)/$(TARGET).lst

$(OUTPUT): $(ELF) build3nr.py stable_abi.py semantic_marker.py binutils_elf.py ../sysmodules/loader/loader.elf ../sysmodules/rosalina/rosalina.elf
	@python3 build3nr.py \
		--fixer-elf $(ELF) \
		--loader-elf ../sysmodules/loader/loader.elf --loader-root ../sysmodules/loader --loader-manifests ../sysmodules/loader/build/semantic_markers \
		--rosalina-elf ../sysmodules/rosalina/rosalina.elf --rosalina-root ../sysmodules/rosalina --rosalina-manifests ../sysmodules/rosalina/build/semantic_markers \
		--output $(OUTPUT) --k11-header $(K11_HEADER)

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(OUTPUT) $(K11_HEADER)
	@find . -type d -name '__pycache__' -prune -exec rm -rf {} +

-include $(DEPS)