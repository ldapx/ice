#!/usr/bin/env bash
# Create a macOS application bundle and deploy Qt/runtime libraries.
#
# Usage: bundle-macos-runtime.sh <xpack-staging-dir>
#
# macdeployqt handles Qt frameworks and plugins. When dylibbundler is
# installed (brew install dylibbundler), it also bundles non-Qt dependencies
# such as ngspice, OpenSSL, spdlog, and fmt so the zip is relocatable.
set -euo pipefail

staging="${1:?usage: bundle-macos-runtime.sh <staging-dir>}"
bindir="$staging/bin"
[[ -d "$bindir" ]] || exit 0

qt_bin="${ICE_QT_BIN:-}"
if [[ -z "$qt_bin" ]]; then
    if command -v qmake6 >/dev/null 2>&1; then
        qt_bin="$(qmake6 -query QT_INSTALL_BINS 2>/dev/null || true)"
    elif command -v qmake >/dev/null 2>&1; then
        qt_bin="$(qmake -query QT_INSTALL_BINS 2>/dev/null || true)"
    fi
fi

app="$staging/ICE.app"
if [[ -x "$bindir/ice" ]]; then
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
    cp "$bindir/ice" "$app/Contents/MacOS/ice"
    chmod +x "$app/Contents/MacOS/ice"

    if [[ -d "$staging/share/ice/symbols" ]]; then
        cp -R "$staging/share/ice/symbols" "$app/Contents/Resources/symbols"
    fi
    if [[ -d "$staging/share/ice/assets" ]]; then
        cp -R "$staging/share/ice/assets" "$app/Contents/Resources/assets"
    fi
    if [[ -d "$staging/share/ice/python" ]]; then
        cp -R "$staging/share/ice/python" "$app/Contents/Resources/python"
    fi

    cat >"$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>ice</string>
  <key>CFBundleIdentifier</key>
  <string>org.ice-eda.ice</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>ICE</string>
  <key>CFBundleDisplayName</key>
  <string>ICE</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1.0</string>
  <key>CFBundleVersion</key>
  <string>0.1.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
PLIST

    if [[ -n "$qt_bin" && -x "$qt_bin/macdeployqt" ]]; then
        "$qt_bin/macdeployqt" "$app" -always-overwrite
    fi

    if command -v dylibbundler >/dev/null 2>&1; then
        dylibbundler -of -b -x "$app/Contents/MacOS/ice" -d "$app/Contents/Frameworks" \
            -p @executable_path/../Frameworks || true
    fi
fi

if command -v dylibbundler >/dev/null 2>&1; then
    mkdir -p "$staging/lib"
    for exe in "$bindir"/*; do
        [[ -f "$exe" && -x "$exe" ]] || continue
        dylibbundler -of -b -x "$exe" -d "$staging/lib" -p @executable_path/../lib || true
    done
fi

echo "Bundled macOS runtime into $staging"
