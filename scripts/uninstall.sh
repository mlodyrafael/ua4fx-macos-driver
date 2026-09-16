#!/bin/bash
# Removes the UA-4FX HAL plug-in (sudo) and the MIDI bridge launch agent.
set -uo pipefail
HAL_DIR=/Library/Audio/Plug-Ins/HAL
AGENT="$HOME/Library/LaunchAgents/com.ua4fx.midid.plist"
APP_SUPPORT="$HOME/Library/Application Support/UA4FX"

launchctl bootout "gui/$(id -u)/com.ua4fx.midid" 2>/dev/null || true
rm -f "$AGENT"
rm -rf "$APP_SUPPORT" "$HOME/Applications/UA4FX Control.app"

if [ -d "$HAL_DIR/UA4FX.driver" ]; then
    echo "==> Removing $HAL_DIR/UA4FX.driver (sudo)"
    sudo rm -rf "$HAL_DIR/UA4FX.driver"
    sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall coreaudiod
fi
echo "done"
