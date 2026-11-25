# =============================================================================
# seq-alchemy — the sequencer on the Hermetic Modular Alchemy Lab V2, played
# from a Launchpad Mini MK3 (+ Launch Control XL, + a Haute42 gamepad) over
# the front USB-C
#
# Standard Daisy workflow (libDaisy core Makefile underneath), run in alchemy/:
#   make libdaisy       — build ../lib/libDaisy once after cloning
#   make                — build firmware -> build/seq_alchemy.bin
#   make program-dfu    — flash over USB (module in DFU mode first)
#   make program-live   — reboot the running module over USB and flash it
#                         (Mac mode only: Launchpad mode has no HostLink)
#   make clean          — remove the build tree
#
# Never run make with a module attached in DFU.
#
# Shared by copy with smack/mark/belt-alchemy and relay: the picker, the
# linker script, launchpad, usbh_hub_midi, usb_shared, tools/hostlink-fs.mjs
# and the shape of this file. A fix in one belongs in all.
# =============================================================================

TARGET = seq_alchemy

# Alchemy Lab board revision: v1 | v2
BOARD ?= v2
ifeq ($(filter $(BOARD),v1 v2),)
$(error BOARD must be 'v1' or 'v2' (got '$(BOARD)'))
endif

ALCHEMY_DIR  = ../lib/alchemy-sdk
LIBDAISY_DIR = ../lib/libDaisy

# ── App sources ─────────────────────────────────────────────────────────────
CPP_SOURCES = \
    src/seq_alchemy.cpp \
    src/picker.cpp \
    src/launchpad.cpp \
    src/usb_shared.cpp \
    src/midi_out.cpp \
    src/phone_ext.cpp \
    src/midi_usb_transport.cpp

C_SOURCES = \
    ../core/seq.c \
    ../core/chord_midi.c \
    src/usbh_hub_midi.c \
    src/uac_none.c

# ── Alchemy SDK, compiled straight from the submodule ───────────────────────
CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))

C_INCLUDES += \
    -Isrc \
    -I../core \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include

ifeq ($(BOARD),v2)
C_DEFS += -DALCHEMY_BOARD_V2
endif

SEQ_VERSION  := $(shell cat ../VERSION 2>/dev/null || echo 0.0.0)
SEQ_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
C_DEFS += -DSEQ_VERSION=\"$(SEQ_VERSION)\" -DSEQ_GIT_HASH=\"$(SEQ_GIT_HASH)\"

# ── Daisy bootloader build (BOOT_SRAM) ──────────────────────────────────────
# FatFS for the SD card (picker, usb.cfg); see mark-alchemy's Makefile.
USE_FATFS = 1

APP_TYPE = BOOT_SRAM
# The SDK's alchemy_stm32h750ib_sram.lds plus the `.axi_bss` section the
# SDK's SD-card code needs. Re-copy from the SDK and re-add it when the
# submodule moves.
LDSCRIPT = src/seq_alchemy.lds

# The Alchemy SDK requires C++17 (libDaisy's default is gnu++14).
CPP_STANDARD = -std=gnu++17

OPT = -O3

# ── libDaisy core Makefile does the rest ────────────────────────────────────
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

.DEFAULT_GOAL := all

# newlib-nano prints NOTHING for "%f" unless _printf_float is linked in.
override LDFLAGS += -u _printf_float

BOARD_STAMP := $(BUILD_DIR)/.board-$(BOARD)
ifeq ($(wildcard $(BOARD_STAMP)),)
_BOARD_GUARD := $(shell rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(BUILD_DIR)/*.lst $(BUILD_DIR)/.board-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(BOARD_STAMP))
endif

# The commit hash is a -D on one compile line and make does not track flags:
# retire that object whenever HEAD moves.
HASH_STAMP := $(BUILD_DIR)/.hash-$(SEQ_GIT_HASH)
ifeq ($(wildcard $(HASH_STAMP)),)
_HASH_GUARD := $(shell rm -f $(BUILD_DIR)/seq_alchemy.o $(BUILD_DIR)/seq_alchemy.d $(BUILD_DIR)/.hash-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(HASH_STAMP))
endif

.PHONY: libdaisy
libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR)

USBPID ?= df11

.PHONY: program-live
program-live: all
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)
