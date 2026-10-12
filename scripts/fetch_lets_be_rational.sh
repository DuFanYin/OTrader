#!/usr/bin/env bash
# Fetch LetsBeRational (Peter Jäckel's implied Black volatility, www.jaeckel.org) into
# engine/thirdparty/lets_be_rational/src, where quant/CMakeLists.txt compiles it.
# It is not in git (thirdparty/ is ignored); its licence lets it be copied and modified as
# long as the notice is kept (README.txt is copied along).
# Usage: ./fetch_lets_be_rational.sh [LetsBeRational.7z]   (a local copy skips the download)
# Needs 7z (apt install 7zip).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/engine/thirdparty/lets_be_rational/src"
URL="http://www.jaeckel.org/LetsBeRational.7z"
# Version 1.0.0.1520 (2024-02-15). A different archive means upstream changed: check it builds.
SHA256="da2f6870b213e04ef35b4d309269ee5ce12be5830d9733e5f29542bf7b652470"
FILES=(lets_be_rational.cpp lets_be_rational.h normaldistribution.cpp normaldistribution.h
       rationalcubic.cpp rationalcubic.h erf_cody.cpp dllmain.h version.h README.txt)

command -v 7z > /dev/null || { echo "Error: 7z not found (apt install 7zip)"; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ARCHIVE="${1:-}"
if [[ -z "$ARCHIVE" ]]; then
  ARCHIVE="$TMP/LetsBeRational.7z"
  echo ">>> Downloading $URL"
  curl -fsSL --retry 3 -o "$ARCHIVE" "$URL"
fi
if ! echo "$SHA256  $ARCHIVE" | sha256sum --check --status; then
  echo "Error: $ARCHIVE does not match the expected SHA-256 ($SHA256)."
  exit 1
fi

7z x -y -o"$TMP/x" "$ARCHIVE" > /dev/null
mkdir -p "$DEST"
for f in "${FILES[@]}"; do
  cp "$TMP/x/LetsBeRational/$f" "$DEST/"
done
# GCC accepts '½' in an identifier, Clang (clangd, clang-tidy) does not; rename the one function
# that has it so both can read the code.
sed -i 's/cdfm½/cdfm_half/g' "$DEST/normaldistribution.cpp"

echo ">>> LetsBeRational in $DEST"
