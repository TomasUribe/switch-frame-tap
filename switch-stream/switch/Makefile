#---------------------------------------------------------------------------------
# devkitA64 / libnx sysmodule Makefile (produces switch-stream.nsp)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set; source /etc/profile.d/devkit-env.sh or install devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET      := switch-stream
BUILD       := build
SOURCES     := source
INCLUDES    := source
CONFIG_JSON := $(TARGET).json

APP_TITLE   := switch-stream
APP_AUTHOR  := you
APP_VERSION := 0.1.0

#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

DEFINES := -D__SWITCH__
CFLAGS  := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections \
           $(ARCH) $(DEFINES) $(INCLUDE) -std=gnu11
CXXFLAGS:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
           -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

LIBS     := -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CC)
export OFILES := $(addsuffix .o,$(basename $(CFILES) $(CPPFILES) $(SFILES)))
export HFILES :=

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export BUILD_EXEFS_SRC := ../exefs_src
export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all : $(OUTPUT).nsp

$(OUTPUT).nsp : $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)
endif
