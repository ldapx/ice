#!/usr/bin/env bash
# Bundle the MSYS2/MinGW runtime next to the staged Windows executables.
#
# Usage: bundle-windows-runtime.sh <xpack-staging-dir>
#
# The script expects ICE_DEPS_PREFIX (or MINGW_PREFIX) to point at the MSYS2
# UCRT64/MINGW64 prefix that provided Qt, ngspice, OpenSSL, and the other
# dependencies. It is safe to run when windeployqt or ntldd are unavailable;
# the corresponding step is skipped.
set -euo pipefail

staging="${1:?usage: bundle-windows-runtime.sh <staging-dir>}"
if command -v cygpath >/dev/null 2>&1; then
    staging="$(cygpath -u "$staging")"
fi
bindir="$staging/bin"
[[ -d "$bindir" ]] || exit 0

normalize_path() {
    local value="$1"
    if command -v cygpath >/dev/null 2>&1 && { [[ "$value" == [A-Za-z]:* ]] || [[ "$value" == *\\* ]]; }; then
        cygpath -u "$value"
    else
        printf '%s\n' "$value"
    fi
}

deps_prefix="${ICE_DEPS_PREFIX:-${MINGW_PREFIX:-}}"
deps_prefix="$(normalize_path "$deps_prefix")"
if [[ -z "$deps_prefix" ]]; then
    for candidate in /ucrt64 /mingw64 /clang64; do
        if [[ -d "$candidate" ]]; then
            deps_prefix="$candidate"
            break
        fi
    done
fi
qt_bin="${ICE_QT_BIN:-${deps_prefix:+$deps_prefix/bin}}"
qt_bin="$(normalize_path "$qt_bin")"

deployqt=""
for candidate in \
    "${qt_bin:+$qt_bin/windeployqt.exe}" \
    "${qt_bin:+$qt_bin/windeployqt6.exe}" \
    "${deps_prefix:+$deps_prefix/lib/qt6/bin/windeployqt.exe}" \
    "${deps_prefix:+$deps_prefix/lib/qt6/bin/windeployqt6.exe}" \
    "$(command -v windeployqt6 2>/dev/null || true)" \
    "$(command -v windeployqt 2>/dev/null || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        deployqt="$candidate"
        break
    fi
done

if [[ -n "$deployqt" && -f "$bindir/ice.exe" ]]; then
    "$deployqt" --release --no-translations --no-compiler-runtime --dir "$bindir" "$bindir/ice.exe"
fi

# Copy every non-system DLL that the staged executables depend on. This covers
# the compiler runtime, ngspice, OpenSSL, zlib, curl, simdjson, and any Qt
# libraries that windeployqt did not handle.
ntldd_cmd="$(command -v ntldd || command -v ntldd.exe || true)"
if [[ -n "$ntldd_cmd" ]]; then
    copy_dependencies() {
        local target="$1"
        "$ntldd_cmd" -R "$target" 2>/dev/null | while IFS= read -r line; do
            [[ "$line" == *"=>"* ]] || continue
            local dll
            dll="$(sed -n 's/.*=> \(.*\) (0x[0-9A-Fa-f]*)$/\1/p' <<<"$line")"
            [[ -n "$dll" ]] || continue
            dll="$(normalize_path "$dll")"
            [[ -f "$dll" ]] || continue
            case "$dll" in
            /c/Windows/* | /c/windows/* | /Windows/* | /c/Program\ Files/* | /c/ProgramData/*)
                continue
                ;;
            esac
            if [[ -n "$deps_prefix" && ( "$dll" == "$deps_prefix"/* || "$dll" == /ucrt64/* ||
                "$dll" == /mingw64/* || "$dll" == /clang64/* ) ]]; then
                cp -n "$dll" "$bindir/" 2>/dev/null || true
            fi
        done
    }
    for exe in "$bindir"/*.exe; do
        [[ -f "$exe" ]] || continue
        copy_dependencies "$exe"
    done
fi

if [[ -n "$deps_prefix" ]]; then
    # Fallback when windeployqt is unavailable.
    for dll in "$deps_prefix"/bin/Qt6*.dll; do
        [[ -f "$dll" ]] && cp -n "$dll" "$bindir/" 2>/dev/null || true
    done
    for plugins_root in "$deps_prefix/share/qt6/plugins" "$deps_prefix/lib/qt6/plugins"; do
        [[ -d "$plugins_root" ]] || continue
        for plugin_dir in platforms styles imageformats iconengines tls; do
            if [[ -d "$plugins_root/$plugin_dir" ]]; then
                cp -R "$plugins_root/$plugin_dir" "$bindir/"
            fi
        done
    done
    for data_dir in lib/ngspice share/ngspice; do
        if [[ -d "$deps_prefix/$data_dir" ]]; then
            mkdir -p "$staging/$(dirname "$data_dir")"
            cp -R "$deps_prefix/$data_dir" "$staging/$data_dir"
        fi
    done
fi

echo "Bundled Windows runtime into $bindir"
