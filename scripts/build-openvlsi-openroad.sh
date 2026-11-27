#!/usr/bin/env bash
# Build the C++ OpenROAD backend used by OpenVLSI.
#
# The OpenROAD and OpenSTA revisions are pinned in
# scripts/openroad-sdk-pins.env. When OPENVLSI_OPENROAD_SRC is not set, the pinned
# source pair is fetched into a fixed per-user cache; no sibling checkout or AUR
# tree is required.
#
# Usage:
#   scripts/build-openvlsi-openroad.sh
#   OPENVLSI_OPENROAD_SRC=/path/to/OpenROAD scripts/build-openvlsi-openroad.sh
#   scripts/build-openvlsi-openroad.sh --install /home/user/.local/bin
#   scripts/build-openvlsi-openroad.sh --output-dir build/linux/x86_64/debug
#   scripts/build-openvlsi-openroad.sh --sdk-install /home/user/.local/openvlsi-openroad
#
# The helper is copied next to openvlsi-cli by default so OpenVLSI and openvlsi-cli discover
# it without extra configuration. Set OPENVLSI_OPENROAD_HELPER to override.
# --sdk-install also installs libopenvlsi_openroad.so, public headers, the harness,
# and a pkg-config file under the given prefix.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pins_file="$root/scripts/openroad-sdk-pins.env"
if [[ ! -f "$pins_file" ]]; then
    echo "missing OpenROAD pin file: $pins_file" >&2
    exit 2
fi
# shellcheck source=scripts/openroad-sdk-pins.env
source "$pins_file"

openroad_src="${OPENVLSI_OPENROAD_SRC:-}"
install_dir=""
sdk_install_dir=""
output_dir=""
jobs="${OPENVLSI_OPENROAD_JOBS:-2}"
refresh=0
fetch_only=0
cache_root="$HOME/.cache/openvlsi/openroad-sdk"
default_dir="$root/build/linux/x86_64/release"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --install <dir>         Also install openvlsi-openroad into <dir>.
  --output-dir <dir>      Install openvlsi-openroad into <dir> instead of
                          build/linux/x86_64/release.
  --sdk-install <dir>     Install the helper, shared library, headers, and
                          pkg-config file under <dir>.
  --jobs <n>              Bazel job count (default: ${jobs}).
  --refresh               Refresh the cached source before building.
  --fetch-only            Fetch the pinned source into the cache and exit.
  -h, --help              Show this help.

Pinned revisions:
  OpenROAD ${OPENROAD_VERSION_LABEL} (${OPENROAD_PIN})
  OpenSTA  ${OPENSTA_PIN}

Environment:
  OPENVLSI_OPENROAD_SRC        Use this OpenROAD source checkout instead of fetching.
  OPENVLSI_OPENROAD_JOBS       Bazel job count.
EOF
}

info() { printf '%s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
    case "$1" in
    --install)
        install_dir="${2:?missing --install directory}"
        shift 2
        ;;
    --output-dir)
        output_dir="${2:?missing --output-dir directory}"
        shift 2
        ;;
    --sdk-install)
        sdk_install_dir="${2:?missing --sdk-install prefix}"
        shift 2
        ;;
    --jobs)
        jobs="${2:?missing --jobs count}"
        shift 2
        ;;
    --refresh)
        refresh=1
        shift
        ;;
    --fetch-only)
        fetch_only=1
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        die "unknown option: $1"
        ;;
    esac
done

openvlsi_version() {
    local version
    version="$(sed -n 's/^set_version("\([^"]*\)").*/\1/p' "$root/xmake.lua" | head -n 1)"
    [[ -n "$version" ]] || die "unable to determine OpenVLSI version from xmake.lua"
    printf '%s\n' "$version"
}

install_helper() {
    local source="$1"
    [[ -x "$source" ]] || die "helper is not executable: $source"
    local helper_dir="${output_dir:-$default_dir}"
    mkdir -p "$helper_dir"
    rm -f "$helper_dir/openvlsi-openroad"
    cp "$source" "$helper_dir/openvlsi-openroad"
    chmod +x "$helper_dir/openvlsi-openroad"
    info "Installed $helper_dir/openvlsi-openroad"
    if [[ -n "$install_dir" ]]; then
        mkdir -p "$install_dir"
        rm -f "$install_dir/openvlsi-openroad"
        cp "$source" "$install_dir/openvlsi-openroad"
        chmod +x "$install_dir/openvlsi-openroad"
        info "Installed $install_dir/openvlsi-openroad"
    fi
}

fetch_openroad_source() {
    local destination="$1"
    local temporary="${destination}.tmp.$$"
    rm -rf "$temporary"
    mkdir -p "$(dirname "$destination")"
    info "Fetching OpenROAD ${OPENROAD_VERSION_LABEL} into $destination"
    info "  OpenROAD: ${OPENROAD_REPOSITORY} @ ${OPENROAD_PIN}"
    info "  OpenSTA:  ${OPENSTA_REPOSITORY} @ ${OPENSTA_PIN}"
    git init -q "$temporary"
    git -C "$temporary" remote add origin "$OPENROAD_REPOSITORY"
    git -C "$temporary" fetch --depth 1 origin "$OPENROAD_PIN"
    git -C "$temporary" checkout -q --detach FETCH_HEAD

    # Let the normal submodule URL work first, then fall back to a separate
    # pinned OpenSTA checkout. The fallback also handles relative submodule
    # URLs that cannot be resolved from a fetched commit.
    if ! git -C "$temporary" -c protocol.file.allow=always submodule update --init --recursive; then
        info "OpenROAD submodule update failed; fetching OpenSTA directly"
    fi
    if [[ ! -f "$temporary/src/sta/BUILD.bazel" ]]; then
        rm -rf "$temporary/src/sta"
        git clone -q "$OPENSTA_REPOSITORY" "$temporary/src/sta"
        git -C "$temporary/src/sta" fetch --depth 1 origin "$OPENSTA_PIN"
        git -C "$temporary/src/sta" checkout -q --detach FETCH_HEAD
    fi
    [[ -f "$temporary/BUILD.bazel" ]] || die "OpenROAD checkout has no BUILD.bazel"
    [[ -f "$temporary/src/sta/BUILD.bazel" ]] || die "OpenSTA checkout has no BUILD.bazel"
    mv "$temporary" "$destination"
}

resolve_openroad_source() {
    if [[ -n "$openroad_src" ]]; then
        [[ -d "$openroad_src" ]] || die "OPENVLSI_OPENROAD_SRC is not a directory: $openroad_src"
        [[ -f "$openroad_src/BUILD.bazel" ]] || die "OpenROAD source has no BUILD.bazel: $openroad_src"
        [[ -f "$openroad_src/src/sta/BUILD.bazel" ]] ||
            die "OpenROAD source is missing the OpenSTA Bazel package (src/sta/BUILD.bazel): $openroad_src"
        printf '%s\n' "$openroad_src"
        return
    fi
    local destination="$cache_root/OpenROAD-${OPENROAD_PIN:0:12}"
    if [[ "$refresh" -eq 1 || ! -d "$destination" ]]; then
        rm -rf "$destination"
        fetch_openroad_source "$destination"
    fi
    [[ -f "$destination/BUILD.bazel" ]] || die "cached OpenROAD source is incomplete; rerun with --refresh"
    [[ -f "$destination/src/sta/BUILD.bazel" ]] ||
        die "cached OpenSTA source is incomplete; rerun with --refresh"
    printf '%s\n' "$destination"
}

run_source_build() {
    local openroad_src_resolved bazel_cmd sdk_target built shared
    openroad_src_resolved="$(resolve_openroad_source)"
    info "Using OpenROAD source: $openroad_src_resolved"
    if [[ "$fetch_only" -eq 1 ]]; then
        info "Pinned source is ready; skipping build because --fetch-only was requested."
        return
    fi
    bazel_cmd="$(command -v bazelisk || command -v bazel || true)"
    [[ -n "$bazel_cmd" ]] || die "bazelisk or bazel is required"

    sdk_target="$openroad_src_resolved/openvlsi_openroad"
    rm -rf "$sdk_target"
    cp -r "$root/openroad_sdk" "$sdk_target"

    local build_targets=(//openvlsi_openroad:openvlsi-openroad)
    if [[ -n "$sdk_install_dir" ]]; then
        build_targets+=(//openvlsi_openroad:openvlsi_openroad_shared)
    fi
    (
        cd "$openroad_src_resolved"
        "$bazel_cmd" build --jobs="$jobs" --config=release --//:platform=cli "${build_targets[@]}"
    )

    built="$openroad_src_resolved/bazel-bin/openvlsi_openroad/openvlsi-openroad"
    [[ -x "$built" ]] || die "build finished but $built is missing"
    install_helper "$built"

    if [[ -n "$sdk_install_dir" ]]; then
        shared="$openroad_src_resolved/bazel-bin/openvlsi_openroad/libopenvlsi_openroad.so"
        [[ -f "$shared" ]] || die "build finished but $shared is missing"
        mkdir -p "$sdk_install_dir/lib" "$sdk_install_dir/bin" "$sdk_install_dir/include/openvlsi_openroad" \
            "$sdk_install_dir/lib/pkgconfig"
        cp "$root"/openroad_sdk/include/openvlsi_openroad/*.hpp "$sdk_install_dir/include/openvlsi_openroad/"
        cp "$shared" "$sdk_install_dir/lib/libopenvlsi_openroad.so"
        cp "$built" "$sdk_install_dir/bin/openvlsi-openroad"
        chmod +x "$sdk_install_dir/bin/openvlsi-openroad"
        cat >"$sdk_install_dir/lib/pkgconfig/openvlsi_openroad.pc" <<EOF
prefix=$sdk_install_dir
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${exec_prefix}/include

Name: openvlsi_openroad
Description: OpenVLSI C++ OpenROAD stage and ODB preview library
Version: $(openvlsi_version)
Libs: -L\${libdir} -lopenvlsi_openroad
Cflags: -I\${includedir}
EOF
        info "Installed SDK under $sdk_install_dir"
    fi
}

run_source_build
