#!/usr/bin/env bash

# Unified clang helper:
#   第一个参数: t = tidy, f = format
#   第二个参数: d = dry-run, f = fix (默认 f)
#
# 示例:
#   ./run-clang.sh f        # clang-format, fix 模式
#   ./run-clang.sh f d      # clang-format, dry-run 仅检查
#   ./run-clang.sh t        # clang-tidy, fix 模式 (--fix --fix-errors)
#   ./run-clang.sh t d      # clang-tidy, dry-run 仅报告

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

usage() {
  cat <<EOF
用法: ./run-clang.sh <mode> [action]

  mode:
    t  clang-tidy
    f  clang-format

  action:
    d  dry-run (只检查，不修改文件)
    f  fix     (应用修改)   [默认]

示例:
  ./run-clang.sh f        # 格式化 C++ 源码 (clang-format -i)
  ./run-clang.sh f d      # 仅检查格式 (clang-format --dry-run --Werror)
  ./run-clang.sh t        # clang-tidy (--fix --fix-errors)
  ./run-clang.sh t d      # clang-tidy 仅检查
EOF
}

MODE="${1:-}"
ACTION="${2:-f}"  # 默认 fix

if [[ "$MODE" != "t" && "$MODE" != "f" ]]; then
  echo "错误: 第一个参数必须是 't' (tidy) 或 'f' (format)" >&2
  usage
  exit 1
fi

if [[ "$ACTION" != "d" && "$ACTION" != "f" ]]; then
  echo "错误: 第二个参数必须是 'd' (dry-run) 或 'f' (fix)" >&2
  usage
  exit 1
fi

cpu_count() {
  if command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8
  else
    echo 8
  fi
}

run_format() {
  local action="$1"  # d | f

  # 优先使用 Homebrew LLVM 的 clang-format
  if [[ -x "/opt/homebrew/opt/llvm/bin/clang-format" ]]; then
    export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
  fi

  # Find all C++ files in Otrader/ excluding build and thirdparty
  local files
  files=$(find Otrader -name "*.cpp" -o -name "*.hpp" | grep -v build | grep -v thirdparty | sort || true)

  if [[ -z "$files" ]]; then
    echo "[run-clang] 没有找到任何 C++ 源文件可格式化。"
    return 0
  fi

  if [[ "$action" == "f" ]]; then
    echo "[run-clang] clang-format: FIX 模式 (in-place)"
    echo "$files" | while read -r file; do
      [[ -z "$file" ]] && continue
      echo "  Formatting: $file"
      clang-format -i "$file"
    done
    echo "[run-clang] clang-format 完成。"
  else
    echo "[run-clang] clang-format: DRY-RUN 模式 (仅检查)"
    echo "  有问题会列出文件，并返回非 0 退出码。"
    echo ""
    local needs_format=false
    echo "$files" | while read -r file; do
      [[ -z "$file" ]] && continue
      if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
        echo "  Needs formatting: $file"
        needs_format=true
      fi
    done
    if [[ "$needs_format" == true ]]; then
      echo ""
      echo "[run-clang] 部分文件需要格式化，请运行: ./run-clang.sh f f"
      exit 1
    else
      echo "[run-clang] 所有文件格式正常。"
    fi
  fi
}

run_tidy() {
  local action="$1"  # d | f

  local llvm_bin="/opt/homebrew/opt/llvm/bin"
  local clang_tidy="${llvm_bin}/clang-tidy"

  if [[ ! -x "$clang_tidy" ]]; then
    echo "Error: clang-tidy not found at $clang_tidy" >&2
    echo "Install with: brew install llvm" >&2
    exit 1
  fi

  local build_dir="$REPO_ROOT/Otrader/build"
  local compile_db="$build_dir/compile_commands.json"

  if [[ ! -f "$compile_db" ]]; then
    echo "Error: compile_commands.json not found:" >&2
    echo "  $compile_db" >&2
    echo "请先构建 C++ 工程 (生成编译数据库):" >&2
    echo "  ./build.sh" >&2
    exit 1
  fi

  local default_dirs=(
    "Otrader/core"
    "Otrader/infra"
    "Otrader/runtime"
    "Otrader/utilities"
    "Otrader/strategy"
  )

  local extra_args=(
    "--extra-arg=-std=c++20"
    "-p" "$build_dir"
    "-quiet"
  )

  local mode="check"
  if [[ "$action" == "f" ]]; then
    mode="fix"
    extra_args+=("--fix" "--fix-errors")
  fi

  echo ">>> Running clang-tidy (MODE=$mode)"
  echo "    compile_commands: $compile_db"

  local jobs
  jobs="$(cpu_count)"

  find "${default_dirs[@]}" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
    ! -path '*/build/*' ! -path '*/thirdparty/*' -print0 \
    | xargs -0 -n 1 -P "$jobs" "$clang_tidy" "${extra_args[@]}"

  echo ">>> clang-tidy $mode completed."
}

if [[ "$MODE" == "f" ]]; then
  run_format "$ACTION"
else
  run_tidy "$ACTION"
fi

