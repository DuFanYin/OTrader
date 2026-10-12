#!/usr/bin/env bash
# Fetch what the IB gateway build (./build.sh r 0 g) compiles in: Interactive Brokers' TWS API (C++ client and
# its .proto messages, GPL-3.0) into engine/thirdparty/IBJts, and the Intel Decimal Floating-Point Math
# Library it needs, built as engine/thirdparty/IntelRDFP/libbid.a. Neither is in git (thirdparty/ is ignored);
# adapters/ib/CMakeLists.txt compiles the API.
# The TWS API comes as IB's installer, run here unattended (it brings its own Java runtime).
# Usage: ./fetch_ibjts.sh [tws-api-stable-linux-x64.sh] [IntelRDFPMathLib20U3.tar.gz]   (local copies skip the downloads)
# Needs curl, gcc and make.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/engine/thirdparty"
# IB's stable release, 1050.02 (2026-09-09). The URL follows the stable release: a different installer means
# a new version, so check the gateway builds with it before changing the version and the hash.
TWS_URL="https://download2.interactivebrokers.com/installers/tws-api/stable/tws-api-stable-linux-x64.sh"
TWS_VERSION="1050.02"
TWS_SHA256="02247a284feda91da4904e5e3d851f20b272124aa91c20ca6097112521ab61df"
# Intel's library, release 2.0 update 3 (2024), from netlib.
INTEL_URL="https://www.netlib.org/misc/intel/IntelRDFPMathLib20U3.tar.gz"
INTEL_SHA256="13f6924b2ed71df9b137a7df98706a0dcc3b43c283a0e32f8b6eadca4305136a"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

check() {  # <file> <sha256>
  if ! echo "$2  $1" | sha256sum --check --status; then
    echo "Error: $1 does not match the expected SHA-256 ($2)."
    exit 1
  fi
}

TWS="${1:-}"
if [[ -z "$TWS" ]]; then
  TWS="$TMP/tws-api.sh"
  echo ">>> Downloading $TWS_URL"
  curl -fsSL --retry 3 -o "$TWS" "$TWS_URL"
fi
check "$TWS" "$TWS_SHA256"
echo ">>> Installing the TWS API into a temporary directory"
sh "$TWS" -q -dir "$TMP/tws" > /dev/null
if ! grep -q "API_Version=$TWS_VERSION" "$TMP/tws/API_VersionNum.txt"; then
  echo "Error: the installer is not TWS API $TWS_VERSION: $(cat "$TMP/tws/API_VersionNum.txt")"
  exit 1
fi
rm -rf "$DEST/IBJts"
mkdir -p "$DEST/IBJts/source"
cp -r "$TMP/tws/source/cppclient" "$TMP/tws/source/proto" "$DEST/IBJts/source/"
cp "$TMP/tws/LICENSE" "$TMP/tws/NOTICE" "$TMP/tws/API_VersionNum.txt" "$DEST/IBJts/"

INTEL="${2:-}"
if [[ -z "$INTEL" ]]; then
  INTEL="$TMP/intel.tar.gz"
  echo ">>> Downloading $INTEL_URL"
  curl -fsSL --retry 3 -o "$INTEL" "$INTEL_URL"
fi
check "$INTEL" "$INTEL_SHA256"
echo ">>> Building libbid.a"
mkdir "$TMP/intel"
tar -xzf "$INTEL" -C "$TMP/intel"
# The flags IB's source/cppclient/Intel_lib_build.txt gives.
make -C "$TMP/intel/LIBRARY" -j"$(getconf _NPROCESSORS_ONLN)" \
  CC=gcc CALL_BY_REF=0 GLOBAL_RND=0 GLOBAL_FLAGS=0 UNCHANGED_BINARY_FLAGS=0 > /dev/null
mkdir -p "$DEST/IntelRDFP"
cp "$TMP/intel/LIBRARY/libbid.a" "$TMP/intel/eula.txt" "$DEST/IntelRDFP/"

echo ">>> TWS API $TWS_VERSION in $DEST/IBJts, libbid.a in $DEST/IntelRDFP"
