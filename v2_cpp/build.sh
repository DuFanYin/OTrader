#!/usr/bin/env bash
# Otrader C++ 纯构建脚本：构建 backtest 与 live 目标（在项目根目录执行）
# 用法: ./build.sh [d|r] [0] [s]
#   d       = Debug，r = Release（默认 Debug）
#   0       = 先 clean 再全量重编；不传 0 则增量 make
#   s       = 仅编译/链接策略相关目标 (strategies + entry_backtest)，不跑全量 make
# 也可用 BUILD_TYPE=Release ./build.sh
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

BUILD_TYPE="${BUILD_TYPE:-Debug}"
BUILD_DIR="${BUILD_DIR:-Otrader/build}"

# Force Homebrew LLVM toolchain (no implicit fallback).
LLVM_BIN="/opt/homebrew/opt/llvm/bin"
if [[ ! -x "$LLVM_BIN/clang" || ! -x "$LLVM_BIN/clang++" ]]; then
  echo "Error: Homebrew LLVM not found."
  echo "Expected:"
  echo "  $LLVM_BIN/clang"
  echo "  $LLVM_BIN/clang++"
  echo ""
  echo "Install:"
  echo "  brew install llvm"
  exit 1
fi

# Ensure we have a macOS SDK path (required for a sane brew-clang toolchain).
SDK_PATH="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
if [[ -z "$SDK_PATH" || ! -d "$SDK_PATH" ]]; then
  echo "Error: macOS SDK not found via xcrun."
  echo "Install Xcode Command Line Tools:"
  echo "  xcode-select --install"
  exit 1
fi

IBJTS_DIR="$REPO_ROOT/Otrader/thirdparty/IBJts"
IBJTS_LIB="$IBJTS_DIR/build-apple/lib/libtwsapi.dylib"
PROTOC_BIN="/opt/homebrew/opt/protobuf/bin/protoc"

if [[ ! -d "$IBJTS_DIR" ]]; then
  echo "Error: Live build requires IBJts, but not found at:"
  echo "  $IBJTS_DIR"
  echo ""
  echo "Please place/extract IBJts there (e.g. twsapi_macunix.1037.02.zip -> IBJts/), then re-run:"
  echo "  ./build.sh"
  exit 1
fi

build_ibjts() {
  if [[ ! -x "$PROTOC_BIN" ]]; then
    echo "Error: protoc not found at $PROTOC_BIN"
    echo "Install with:"
    echo "  brew install protobuf"
    exit 1
  fi
  echo ">>> Building IBJts twsapi (Apple Clang + brew protobuf) into build-apple..."
  mkdir -p "$IBJTS_DIR/build-apple"
  (
    cd "$IBJTS_DIR"
    "$PROTOC_BIN" -I source/proto \
      --cpp_out=source/cppclient/client/protobufUnix \
      source/proto/*.proto
    cd build-apple
    CC=/usr/bin/clang CXX=/usr/bin/clang++ \
      cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew ..
    cmake --build . -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"
  ) || {
    echo "Error: failed to build IBJts twsapi (see messages above)."
    exit 1
  }
}

if [[ ! -f "$IBJTS_LIB" ]]; then
  build_ibjts
fi

DO_CLEAN=0
STRATEGY_ONLY=0
for a in "$@"; do
  case "$a" in
    d) BUILD_TYPE=Debug ;;
    r) BUILD_TYPE=Release ;;
    0) DO_CLEAN=1 ;;
    s) STRATEGY_ONLY=1 ;;
  esac
done

cmake_configure() {
  LLVM_LIBCXX_LIBDIR="/opt/homebrew/opt/llvm/lib/c++"
  if [[ ! -d "$LLVM_LIBCXX_LIBDIR" ]]; then
    echo "Error: Homebrew libc++ directory not found:"
    echo "  $LLVM_LIBCXX_LIBDIR"
    echo "Reinstall llvm:"
    echo "  brew reinstall llvm"
    exit 1
  fi

  cmake \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_BACKTEST=ON \
    -DBUILD_TESTS=ON \
    -DBUILD_LIVE_GRPC=ON \
    -DBUILD_GATEWAY=ON \
    -DBUILD_MARKETDATA=ON \
    -DCMAKE_CXX_COMPILER="$LLVM_BIN/clang++" \
    -DCMAKE_OSX_SYSROOT="$SDK_PATH" \
    -DCMAKE_PREFIX_PATH="$REPO_ROOT/cmake;/opt/homebrew/opt/zlib;/opt/homebrew" \
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/anaconda3" \
    -DZLIB_ROOT="/opt/homebrew/opt/zlib" \
    -DCMAKE_CXX_FLAGS="-gdwarf-4" \
    -DCMAKE_EXE_LINKER_FLAGS="-L${LLVM_LIBCXX_LIBDIR} -Wl,-rpath,${LLVM_LIBCXX_LIBDIR}" \
    -DCMAKE_SHARED_LINKER_FLAGS="-L${LLVM_LIBCXX_LIBDIR} -Wl,-rpath,${LLVM_LIBCXX_LIBDIR}" \
    ..
}

if [[ ! -d "$BUILD_DIR" ]]; then
  echo ">>> Creating $BUILD_DIR and running cmake (backtest + live)..."
  mkdir -p "$BUILD_DIR"
  ( cd "$BUILD_DIR" && cmake_configure )
  DO_CLEAN=0
fi

if [[ "$DO_CLEAN" -eq 1 ]]; then
  echo ">>> Reconfigure + clean (BUILD_BACKTEST=ON, BUILD_LIVE=ON)"
  ( cd "$BUILD_DIR" && cmake_configure && make clean 2>/dev/null || true )
fi

if [[ "$STRATEGY_ONLY" -eq 1 ]]; then
  echo ">>> strategy-only build in $BUILD_DIR (strategies + entry_backtest)"
  (
    cd "$BUILD_DIR"
    cmake --build . --target strategies -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"
    cmake --build . --target entry_backtest -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"
  )
  echo ""
  echo "========================================"
  echo "Otrader strategies build done: $BUILD_DIR"
  echo "  libstrategies.a (strategy library)"
  echo "  entry_backtest  = $BUILD_DIR/entry_backtest"
  echo "========================================"
else
  # 只编聚合 target otrader_main（entry_backtest + entry_system 一次构建，无重复进度）
  echo ">>> Building main targets (otrader_main) in $BUILD_DIR (CMAKE_BUILD_TYPE=$BUILD_TYPE)"
  ( cd "$BUILD_DIR" && cmake --build . --target otrader_main \
    -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" )

  echo ""
  echo "========================================"
  echo "Otrader C++ build done: $BUILD_DIR"
  echo "  entry_backtest   = $BUILD_DIR/entry_backtest"
  echo "  entry_system     = $BUILD_DIR/entry_system  (modes: --mode=gateway|market|live|all)"
  echo "  (test targets: ./test_otrader.sh build | ./test_gtest.sh build)"
  echo "========================================"
fi
