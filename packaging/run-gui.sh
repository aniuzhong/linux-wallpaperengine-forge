#!/usr/bin/env bash
# 套件启动入口: 把包内 bin/ 前置到 PATH, GUI 后端即可按名字找到包内引擎;
# V11 桌面集成 (透明壁纸 + 引擎 layer 时序) 由 GUI 后端 (pkg/background) 自治, 无需设置。
# 用法: ./run-gui.sh [--minimized] [其它 GUI 参数原样透传]
set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
exec env PATH="$DIR/bin:$PATH" "$DIR/gui/linux-wallpaperengine-gui" "$@"
