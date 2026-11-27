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

qt_bin="${OPENVLSI_QT_BIN:-}"
if [[ -z "$qt_bin" ]]; then
    if command -v qmake6 >/dev/null 2>&1; then
        qt_bin="$(qmake6 -query QT_INSTALL_BINS 2>/dev/null || true)"
    elif command -v qmake >/dev/null 2>&1; then
        qt_bin="$(qmake -query QT_INSTALL_BINS 2>/dev/null || true)"
    fi
fi

app="$staging/OpenVLSI.app"
if [[ -x "$bindir/openvlsi" ]]; then
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
    cp "$bindir/openvlsi" "$app/Contents/MacOS/openvlsi"
    chmod +x "$app/Contents/MacOS/openvlsi"

    if [[ -d "$staging/share/openvlsi/symbols" ]]; then
        cp -R "$staging/share/openvlsi/symbols" "$app/Contents/Resources/symbols"
    fi
    if [[ -d "$staging/share/openvlsi/assets" ]]; then
        cp -R "$staging/share/openvlsi/assets" "$app/Contents/Resources/assets"
    fi
    if [[ -d "$staging/share/openvlsi/python" ]]; then
        cp -R "$staging/share/openvlsi/python" "$app/Contents/Resources/python"
    fi

    cat >"$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>openvlsi</string>
  <key>CFBundleIdentifier</key>
  <string>org.openvlsi-eda.openvlsi</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>OpenVLSI</string>
  <key>CFBundleDisplayName</key>
  <string>OpenVLSI</string>
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
        dylibbundler -of -b -x "$app/Contents/MacOS/openvlsi" -d "$app/Contents/Frameworks" \
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
