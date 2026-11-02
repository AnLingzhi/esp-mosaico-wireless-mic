#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT="$PWD/.codex-runs/validation/settings"
APP="$OUT/Mosaico Key Settings.app"
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/Resources"
swift tools/macos/make_icon.swift "$OUT"
iconutil -c icns "$OUT/Mosaico.iconset" -o "$APP/Contents/Resources/Mosaico.icns"
clang -fobjc-arc -Wall -Wextra -Werror -framework AppKit -framework Foundation \
    -framework IOKit -framework ApplicationServices tools/macos/mosaico_fn.m -o "$APP/Contents/MacOS/mosaico-fn"
python3 - "$APP" <<'PY'
import pathlib
import plistlib
import sys
path = pathlib.Path(sys.argv[1]) / 'Contents' / 'Info.plist'
path.write_bytes(plistlib.dumps({
    'CFBundleExecutable': 'mosaico-fn',
    'CFBundleIdentifier': 'local.mosaico.key-settings',
    'CFBundleName': 'Mosaico Key Settings',
    'CFBundlePackageType': 'APPL',
    'CFBundleDisplayName': 'Mosaico 前置按键设置',
    'CFBundleDevelopmentRegion': 'zh_CN',
    'CFBundleShortVersionString': '0.3.0',
    'CFBundleVersion': '4',
    'CFBundleIconFile': 'Mosaico.icns',
    'NSHighResolutionCapable': True,
}))
PY
codesign --force --sign - "$APP"
"$APP/Contents/MacOS/mosaico-fn" --test-config
"$APP/Contents/MacOS/mosaico-fn" --test-events
"$APP/Contents/MacOS/mosaico-fn" --test-log
"$APP/Contents/MacOS/mosaico-fn" --test-heartbeat
printf '%s\n' "$APP"
