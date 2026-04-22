#!/usr/bin/env bash
# 开发环境一键初始化脚本
# 用法：./scripts/dev-setup.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "==> [1/4] 编译 C++ sidecar..."
bash "$SCRIPT_DIR/build-sidecar-macos.sh"

echo "==> [2/4] 安装 npm 依赖..."
cd "$PROJECT_ROOT"
npm install

echo "==> [3/4] 检查 Rust 工具链..."
if ! command -v rustc &>/dev/null; then
    echo "    [提示] 未检测到 Rust，正在安装 rustup..."
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    source "$HOME/.cargo/env"
fi
rustc --version
cargo --version

echo "==> [4/4] 检查 Tauri CLI..."
if ! cargo tauri --version &>/dev/null 2>&1; then
    echo "    安装 tauri-cli..."
    cargo install tauri-cli --version "^2"
fi

echo ""
echo "✅ 开发环境准备完毕！"
echo "   运行 npm run tauri dev  启动开发服务器"
echo "   运行 npm run tauri build  打包应用"
