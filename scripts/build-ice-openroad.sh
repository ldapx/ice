#!/usr/bin/env bash
# Build the experimental C++ OpenROAD backend.
#
# Prerequisites:
#   - an OpenROAD source checkout with Bazel support;
#   - bazelisk or bazel on PATH;
#   - an existing Bazel cache or enough time for the first build.
#
# Usage:
#   ICE_OPENROAD_SRC=/path/to/OpenROAD scripts/build-ice-openroad.sh
#   scripts/build-ice-openroad.sh --install /home/user/.local/bin
#   scripts/build-ice-openroad.sh --sdk-install /home/user/.local/ice-openroad
#
# The helper is copied next to ice-cli by default so ICE and ice-cli discover
# it without extra configuration. Set ICE_OPENROAD_HELPER to override.
# --sdk-install also installs libice_openroad.so, public headers, the harness,
# and a pkg-config file under the given prefix.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
openroad_src="${ICE_OPENROAD_SRC:-}"
install_dir=""
sdk_install_dir=""
jobs="${ICE_OPENROAD_JOBS:-2}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install)
      install_dir="${2:?missing --install directory}"
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
    *)
      echo "unknown option: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$openroad_src" ]]; then
  for candidate in \
    "$root/../OpenROAD" \
    "$root/../aur-openroad-git-bazel/src/OpenROAD"; do
    if [[ -d "$candidate" && -f "$candidate/BUILD.bazel" ]]; then
      openroad_src="$candidate"
      break
    fi
  done
fi

if [[ -z "$openroad_src" || ! -f "$openroad_src/BUILD.bazel" ]]; then
  echo "Set ICE_OPENROAD_SRC to an OpenROAD source checkout with BUILD.bazel" >&2
  exit 2
fi

bazel_cmd="$(command -v bazelisk || command -v bazel || true)"
if [[ -z "$bazel_cmd" ]]; then
  echo "bazelisk or bazel is required" >&2
  exit 2
fi

sdk_target="$openroad_src/ice_openroad"
rm -rf "$sdk_target"
cp -r "$root/openroad_sdk" "$sdk_target"

build_targets=(//ice_openroad:ice-openroad)
if [[ -n "$sdk_install_dir" ]]; then
  build_targets+=(//ice_openroad:ice_openroad_shared)
fi

(
  cd "$openroad_src"
  "$bazel_cmd" build --jobs="$jobs" --config=release --//:platform=cli "${build_targets[@]}"
)

built="$openroad_src/bazel-bin/ice_openroad/ice-openroad"
if [[ ! -x "$built" ]]; then
  echo "Build finished but $built is missing" >&2
  exit 1
fi

default_dir="$root/build/linux/x86_64/release"
mkdir -p "$default_dir"
rm -f "$default_dir/ice-openroad"
cp "$built" "$default_dir/ice-openroad"
chmod +x "$default_dir/ice-openroad"
echo "Installed $default_dir/ice-openroad"

if [[ -n "$install_dir" ]]; then
  mkdir -p "$install_dir"
  rm -f "$install_dir/ice-openroad"
  cp "$built" "$install_dir/ice-openroad"
  chmod +x "$install_dir/ice-openroad"
  echo "Installed $install_dir/ice-openroad"
fi

if [[ -n "$sdk_install_dir" ]]; then
  shared="$openroad_src/bazel-bin/ice_openroad/libice_openroad.so"
  if [[ ! -f "$shared" ]]; then
    echo "Build finished but $shared is missing" >&2
    exit 1
  fi
  mkdir -p "$sdk_install_dir/lib" "$sdk_install_dir/bin" "$sdk_install_dir/include/ice_openroad" \
    "$sdk_install_dir/lib/pkgconfig"
  cp "$root"/openroad_sdk/include/ice_openroad/*.hpp "$sdk_install_dir/include/ice_openroad/"
  cp "$shared" "$sdk_install_dir/lib/libice_openroad.so"
  cp "$built" "$sdk_install_dir/bin/ice-openroad"
  chmod +x "$sdk_install_dir/bin/ice-openroad"
  cat > "$sdk_install_dir/lib/pkgconfig/ice_openroad.pc" <<EOF
prefix=$sdk_install_dir
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: ice_openroad
Description: ICE C++ OpenROAD stage and ODB preview library
Version: 0.1.0
Libs: -L\${libdir} -lice_openroad
Cflags: -I\${includedir}
EOF
  echo "Installed SDK under $sdk_install_dir"
fi
