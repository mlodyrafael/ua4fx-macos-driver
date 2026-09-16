#!/bin/bash
# Installs the UA-4FX HAL plug-in (needs sudo) and the MIDI bridge launch agent (user).
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE=build/UA4FX.driver
MIDID=build/ua4fx_midid
HAL_DIR=/Library/Audio/Plug-Ins/HAL
AGENT_DIR="$HOME/Library/LaunchAgents"
APP_SUPPORT="$HOME/Library/Application Support/UA4FX"

[ -d "$BUNDLE" ] || { echo "build first: make"; exit 1; }

echo "==> Installing $BUNDLE to $HAL_DIR (sudo)"
sudo rm -rf "$HAL_DIR/UA4FX.driver"
sudo cp -R "$BUNDLE" "$HAL_DIR/"
sudo chown -R root:wheel "$HAL_DIR/UA4FX.driver"
sudo chmod -R go-w "$HAL_DIR/UA4FX.driver"

echo "==> Restarting coreaudiod"
# launchctl kickstart is refused under SIP on recent macOS; launchd relaunches coreaudiod after a kill.
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall coreaudiod

if [ -x "$MIDID" ]; then
    echo "==> Installing MIDI bridge launch agent (no sudo)"
    mkdir -p "$APP_SUPPORT" "$AGENT_DIR"
    launchctl bootout "gui/$(id -u)/com.ua4fx.midid" 2>/dev/null || true
    cp "$MIDID" "$APP_SUPPORT/ua4fx_midid"
    sed "s|__MIDID__|$APP_SUPPORT/ua4fx_midid|" midi/com.ua4fx.midid.plist > "$AGENT_DIR/com.ua4fx.midid.plist"
    launchctl bootstrap "gui/$(id -u)" "$AGENT_DIR/com.ua4fx.midid.plist"
fi

if [ -d "build/UA4FX Control.app" ]; then
    echo "==> Installing UA4FX Control.app to ~/Applications"
    mkdir -p "$HOME/Applications"
    rm -rf "$HOME/Applications/UA4FX Control.app"
    cp -R "build/UA4FX Control.app" "$HOME/Applications/"
fi

sleep 2
echo "==> Audio devices now:"
system_profiler SPAudioDataType 2>/dev/null | grep -A6 "UA-4FX" || echo "   (UA-4FX not listed yet — check: log show --last 2m --predicate 'subsystem == \"com.ua4fx.driver\"')"
