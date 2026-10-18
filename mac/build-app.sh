#!/bin/bash
# Builds build/InstaPunch-Bridge-Mac.app (ad-hoc signed). Usage: ./build-app.sh [debug|release] [--open]
set -euo pipefail
cd "$(dirname "$0")"
CONFIG="${1:-debug}"
swift build -c "$CONFIG"
BIN="$(swift build -c "$CONFIG" --show-bin-path)"
APP=build/InstaPunch-Bridge-Mac.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN/BridgeMac" "$APP/Contents/MacOS/"
cp Info.plist "$APP/Contents/"
codesign --force --sign - "$APP"
echo "$APP"
if [[ "${2:-}" == "--open" ]]; then open "$APP"; fi
