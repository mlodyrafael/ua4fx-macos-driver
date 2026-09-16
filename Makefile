# UA-4FX user-space driver for macOS (Apple silicon / Intel)
#
#   make            build everything into ./build
#   make driver     CoreAudio HAL plug-in  -> build/UA4FX.driver
#   make midi       CoreMIDI bridge daemon -> build/ua4fx_midid
#   make tools      engine test + HAL harness
#   make install    (asks for sudo) install the HAL plug-in + MIDI launch agent
#   make uninstall
#
CC       ?= clang
ARCHS    ?= $(shell uname -m)
MINOS    ?= 13.0
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fblocks \
            -mmacosx-version-min=$(MINOS) $(addprefix -arch ,$(ARCHS))
FW_BASE   = -framework CoreFoundation -framework IOKit
BUILD     = build
BUNDLE    = $(BUILD)/UA4FX.driver
ENGINE    = driver/UA4FX_USB.c
HDRS      = driver/UA4FX_USB.h driver/UA4FX_Log.h

all: driver midi tools ui

driver: $(BUNDLE)/Contents/MacOS/UA4FX

$(BUNDLE)/Contents/MacOS/UA4FX: driver/UA4FX_Driver.c $(ENGINE) $(HDRS) driver/Info.plist
	@mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	cp driver/Info.plist $(BUNDLE)/Contents/Info.plist
	$(CC) $(CFLAGS) -bundle -o $@ driver/UA4FX_Driver.c $(ENGINE) $(FW_BASE) -framework CoreAudio
	codesign --force --sign - $(BUNDLE)

midi: $(BUILD)/ua4fx_midid

$(BUILD)/ua4fx_midid: midi/ua4fx_midid.c $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DUA4FX_LOG_STDERR=1 -o $@ midi/ua4fx_midid.c $(FW_BASE) -framework CoreMIDI
	codesign --force --sign - $@

tools: $(BUILD)/ua4fx_test $(BUILD)/hal_harness

$(BUILD)/ua4fx_test: tools/ua4fx_test.c $(ENGINE) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DUA4FX_LOG_STDERR=1 -o $@ tools/ua4fx_test.c $(ENGINE) $(FW_BASE)

$(BUILD)/hal_harness: tools/hal_harness.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/hal_harness.c $(FW_BASE) -framework CoreAudio

UIAPP = $(BUILD)/UA4FX\ Control.app
ui: $(BUILD)/UA4FX\ Control.app/Contents/MacOS/UA4FXControl

$(BUILD)/UA4FX\ Control.app/Contents/MacOS/UA4FXControl: ui/UA4FXControl.swift ui/Info.plist
	@mkdir -p "$(BUILD)/UA4FX Control.app/Contents/MacOS" "$(BUILD)/UA4FX Control.app/Contents/Resources"
	cp ui/Info.plist "$(BUILD)/UA4FX Control.app/Contents/Info.plist"
	swiftc -O -parse-as-library -target $(firstword $(ARCHS))-apple-macos$(MINOS) -o "$@" ui/UA4FXControl.swift -framework SwiftUI -framework CoreAudio -framework CoreMIDI -framework AppKit
	codesign --force --sign - "$(BUILD)/UA4FX Control.app"

install: driver midi ui
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

clean:
	rm -rf $(BUILD)

.PHONY: all driver midi tools ui install uninstall clean
