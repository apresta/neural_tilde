#!/bin/bash

set -euo pipefail

MAX_VERSION=9
EXTERNAL_NAME=neural
EXTERNALS_PATH=~/Documents/Max\ ${MAX_VERSION}/Packages/${EXTERNAL_NAME}/externals

LLVM_MINGW_DIR="deps/llvm-mingw"
LLVM_MINGW_SENTINEL="${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-clang"

# Clean build artifacts if requested.
if [[ "${NT_CLEAN:-0}" == "1" ]]; then
  echo "==> Cleaning build artifacts"
  rm -rf build/ CMakeFiles/ .cache/
  rm -f CMakeCache.txt Makefile cmake_install.cmake
fi

# Add any missing submodules.
echo "==> Checking submodules"
SUBMODULE_PATHS=(
  "deps/min-devkit"
  "deps/NeuralAudio"
  "deps/AudioDSPTools"
)
SUBMODULE_URLS=(
  "https://github.com/Cycling74/min-devkit.git"
  "https://github.com/mikeoliphant/NeuralAudio.git"
  "https://github.com/sdatkinson/AudioDSPTools.git"
)
for i in "${!SUBMODULE_PATHS[@]}"; do
  path="${SUBMODULE_PATHS[$i]}"
  url="${SUBMODULE_URLS[$i]}"
  if [[ ! -f "${path}/.git" ]]; then
    echo "  Adding missing submodule: ${path}"
    git submodule add -f "${url}" "${path}"
  fi
done

echo "==> Updating submodules"
git submodule update --init --recursive

# Fetch llvm-mingw cross-compiler.
if [[ ! -x "${LLVM_MINGW_SENTINEL}" ]]; then
  echo "==> Resolving latest llvm-mingw release"
  LLVM_MINGW_VERSION=$(
    curl -fsSL https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest \
      | grep '"tag_name"' \
      | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/'
  )
  LLVM_MINGW_ARCHIVE="llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-macos-universal.tar.xz"
  LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${LLVM_MINGW_ARCHIVE}"

  echo "==> Fetching llvm-mingw ${LLVM_MINGW_VERSION}"
  LLVM_MINGW_TMP="$(mktemp -t llvm-mingw-XXXXXX).tar.xz"
  trap 'rm -f "${LLVM_MINGW_TMP}"' EXIT

  curl -fL --progress-bar "${LLVM_MINGW_URL}" -o "${LLVM_MINGW_TMP}"

  echo "==> Unpacking llvm-mingw into ${LLVM_MINGW_DIR}/"
  mkdir -p "${LLVM_MINGW_DIR}"
  tar -xJf "${LLVM_MINGW_TMP}" -C "${LLVM_MINGW_DIR}" --strip-components=1

  if [[ ! -x "${LLVM_MINGW_SENTINEL}" ]]; then
    echo "ERROR: llvm-mingw unpacked but expected binary not found:" >&2
    echo "  ${LLVM_MINGW_SENTINEL}" >&2
    exit 1
  fi
  echo "  llvm-mingw ready at ${LLVM_MINGW_DIR}/"
else
  echo "==> llvm-mingw already present (${LLVM_MINGW_DIR}/)"
fi

# Build external.
echo "==> Building external"
mkdir -p build
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLLVM_MINGW_PREFIX="$(pwd)/${LLVM_MINGW_DIR}"
cmake --build build --config Release

codesign --force --deep --sign - "${EXTERNALS_PATH}/${EXTERNAL_NAME}~.mxo"

echo
echo "Done."
echo "  macOS:   ${EXTERNALS_PATH}/${EXTERNAL_NAME}~.mxo"
echo "  Windows: ${EXTERNALS_PATH}/${EXTERNAL_NAME}~.mxe64"
