#!/usr/bin/env bash
# 编译 macOS sidecar 二进制
# 在 macOS 上运行此脚本（支持 Apple Silicon 和 Intel）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CPP_DIR="$(dirname "$PROJECT_ROOT")/code"
BINARIES_DIR="$PROJECT_ROOT/src-tauri/binaries"

echo "==> 检测当前架构..."
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ]; then
    TRIPLE="aarch64-apple-darwin"
else
    TRIPLE="x86_64-apple-darwin"
fi
echo "    架构: $ARCH → triple: $TRIPLE"

echo "==> 编译 C++ optimal_sample..."
cd "$CPP_DIR"
g++ -std=c++17 -O2 -o optimal_sample SampleSelectSystem.cpp ILPSolver.cpp main.cpp

echo "==> 复制到 binaries 目录..."
mkdir -p "$BINARIES_DIR"
cp optimal_sample "$BINARIES_DIR/optimal_sample-$TRIPLE"
chmod +x "$BINARIES_DIR/optimal_sample-$TRIPLE"

echo "==> 完成！已生成: $BINARIES_DIR/optimal_sample-$TRIPLE"
