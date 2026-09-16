#!/bin/bash
# One-line installer for the UA-4FX macOS driver.
#
#   curl -fsSL https://raw.githubusercontent.com/mlodyrafael/ua4fx-macos-driver/main/install.sh | bash
#
# What it does:
#   1. checks macOS + Xcode Command Line Tools (offers to install them)
#   2. clones (or updates) the repo into ~/ua4fx-macos-driver
#   3. builds everything locally (so nothing is quarantined by Gatekeeper)
#   4. installs the HAL plug-in (asks for your password once), the MIDI bridge
#      launch agent and UA4FX Control.app, then restarts coreaudiod
set -euo pipefail

REPO_URL="https://github.com/mlodyrafael/ua4fx-macos-driver.git"
DEST="${UA4FX_DIR:-$HOME/ua4fx-macos-driver}"

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "this driver is for macOS only"
major=$(sw_vers -productVersion | cut -d. -f1)
[ "$major" -ge 13 ] || die "macOS 13 or newer is required (found $(sw_vers -productVersion))"

if ! xcode-select -p >/dev/null 2>&1 || ! command -v clang >/dev/null 2>&1 || ! command -v swiftc >/dev/null 2>&1; then
    say "Xcode Command Line Tools are required. Starting the installer…"
    xcode-select --install 2>/dev/null || true
    die "finish the Command Line Tools installation (the dialog that just opened), then run this script again"
fi

# If we are already inside a checkout, use it; otherwise clone/update.
if [ -f "./Makefile" ] && [ -d "./driver" ] && grep -q "UA4FX" ./Makefile 2>/dev/null; then
    DEST="$(pwd)"
    say "Using current checkout: $DEST"
elif [ -d "$DEST/.git" ]; then
    say "Updating $DEST"
    git -C "$DEST" pull --ff-only
else
    say "Cloning into $DEST"
    git clone --depth 1 "$REPO_URL" "$DEST"
fi
cd "$DEST"

say "Building (driver, MIDI bridge, control app)"
make -s all

say "Installing (sudo needed for /Library/Audio/Plug-Ins/HAL)"
./scripts/install.sh

cat <<EOF

Done.
  * Plug the UA-4FX in with its rear switch on ADVANCED. It appears as "EDIROL UA-4FX"
    in Sound settings / Audio MIDI Setup; MIDI ports are "UA-4FX MIDI In/Out".
  * Settings & live stats: ~/Applications/UA4FX Control.app
  * Uninstall:  cd "$DEST" && make uninstall
EOF
