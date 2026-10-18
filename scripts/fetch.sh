#!/usr/bin/env bash
# Fetch the third-party code the build compiles in, into engine/thirdparty (not in git: thirdparty/ is
# ignored). build.sh runs it the first time a build needs one. Local copies skip the downloads.
#
#   ./fetch.sh lets_be_rational [LetsBeRational.7z]
#       LetsBeRational, Peter Jäckel's implied Black volatility (www.jaeckel.org), into
#       engine/thirdparty/lets_be_rational/src, where quant/CMakeLists.txt compiles it. Its licence lets
#       it be copied and modified as long as the notice is kept (README.txt is copied along). Needs 7z.
#   ./fetch.sh ibjts [tws-api-stable-linux-x64.sh] [IntelRDFPMathLib20U3.tar.gz]
#       What the IB gateway build (./build.sh r 0 g) compiles in: Interactive Brokers' TWS API (C++ client
#       and its .proto messages, GPL-3.0) into engine/thirdparty/IBJts, and the Intel Decimal
#       Floating-Point Math Library it needs, built as engine/thirdparty/IntelRDFP/libbid.a;
#       adapters/ib_api/CMakeLists.txt compiles the API. The TWS API comes as IB's installer, run here
#       unattended (it brings its own Java runtime). Needs curl, gcc and make.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRDPARTY="$REPO_ROOT/engine/thirdparty"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

check() {  # <file> <sha256>
  if ! echo "$2  $1" | sha256sum --check --status; then
    echo "Error: $1 does not match the expected SHA-256 ($2)."
    exit 1
  fi
}

download() {  # <url> <file>
  echo ">>> Downloading $1"
  curl -fsSL --retry 3 -o "$2" "$1"
}

lets_be_rational() {
  local dest="$THIRDPARTY/lets_be_rational/src" url="http://www.jaeckel.org/LetsBeRational.7z"
  # Version 1.0.0.1520 (2024-02-15). A different archive means upstream changed: check it builds.
  local sha256="da2f6870b213e04ef35b4d309269ee5ce12be5830d9733e5f29542bf7b652470"
  local files=(lets_be_rational.cpp lets_be_rational.h normaldistribution.cpp normaldistribution.h
               rationalcubic.cpp rationalcubic.h erf_cody.cpp dllmain.h version.h README.txt)
  command -v 7z > /dev/null || { echo "Error: 7z not found (apt install 7zip)"; exit 1; }
  local archive="${1:-}" f
  if [[ -z "$archive" ]]; then archive="$TMP/LetsBeRational.7z"; download "$url" "$archive"; fi
  check "$archive" "$sha256"
  7z x -y -o"$TMP/x" "$archive" > /dev/null
  mkdir -p "$dest"
  for f in "${files[@]}"; do cp "$TMP/x/LetsBeRational/$f" "$dest/"; done
  # GCC accepts '½' in an identifier, Clang (clangd, clang-tidy) does not; rename the one function
  # that has it so both can read the code.
  sed -i 's/cdfm½/cdfm_half/g' "$dest/normaldistribution.cpp"
  echo ">>> LetsBeRational in $dest"
}

ibjts() {
  # IB's stable release, 1050.02 (2026-09-09). The URL follows the stable release: a different installer
  # means a new version, so check the gateway builds with it before changing the version and the hash.
  local tws_url="https://download2.interactivebrokers.com/installers/tws-api/stable/tws-api-stable-linux-x64.sh"
  local tws_version="1050.02"
  local tws_sha256="02247a284feda91da4904e5e3d851f20b272124aa91c20ca6097112521ab61df"
  # Intel's library, release 2.0 update 3 (2024), from netlib.
  local intel_url="https://www.netlib.org/misc/intel/IntelRDFPMathLib20U3.tar.gz"
  local intel_sha256="13f6924b2ed71df9b137a7df98706a0dcc3b43c283a0e32f8b6eadca4305136a"

  local tws="${1:-}"
  if [[ -z "$tws" ]]; then tws="$TMP/tws-api.sh"; download "$tws_url" "$tws"; fi
  check "$tws" "$tws_sha256"
  echo ">>> Installing the TWS API into a temporary directory"
  sh "$tws" -q -dir "$TMP/tws" > /dev/null
  if ! grep -q "API_Version=$tws_version" "$TMP/tws/API_VersionNum.txt"; then
    echo "Error: the installer is not TWS API $tws_version: $(cat "$TMP/tws/API_VersionNum.txt")"
    exit 1
  fi
  rm -rf "$THIRDPARTY/IBJts"
  mkdir -p "$THIRDPARTY/IBJts/source"
  cp -r "$TMP/tws/source/cppclient" "$TMP/tws/source/proto" "$THIRDPARTY/IBJts/source/"
  cp "$TMP/tws/LICENSE" "$TMP/tws/NOTICE" "$TMP/tws/API_VersionNum.txt" "$THIRDPARTY/IBJts/"

  local intel="${2:-}"
  if [[ -z "$intel" ]]; then intel="$TMP/intel.tar.gz"; download "$intel_url" "$intel"; fi
  check "$intel" "$intel_sha256"
  echo ">>> Building libbid.a"
  mkdir "$TMP/intel"
  tar -xzf "$intel" -C "$TMP/intel"
  # The flags IB's source/cppclient/Intel_lib_build.txt gives.
  make -C "$TMP/intel/LIBRARY" -j"$(getconf _NPROCESSORS_ONLN)" \
    CC=gcc CALL_BY_REF=0 GLOBAL_RND=0 GLOBAL_FLAGS=0 UNCHANGED_BINARY_FLAGS=0 > /dev/null
  mkdir -p "$THIRDPARTY/IntelRDFP"
  cp "$TMP/intel/LIBRARY/libbid.a" "$TMP/intel/eula.txt" "$THIRDPARTY/IntelRDFP/"
  echo ">>> TWS API $tws_version in $THIRDPARTY/IBJts, libbid.a in $THIRDPARTY/IntelRDFP"
}

case "${1:-}" in
  lets_be_rational) shift; lets_be_rational "$@" ;;
  ibjts) shift; ibjts "$@" ;;
  *) echo "usage: $0 lets_be_rational [archive] | ibjts [tws installer] [intel archive]" >&2; exit 2 ;;
esac
