#!/usr/bin/env bash
# 套件启动入口: 把包内 bin/ 前置到 PATH, GUI 后端即可按名字找到包内引擎;
# 注入库 (lib/libpeony-alpha.so) 由 GUI 后端按套件布局自动定位, 无需设置。
# 用法: ./run-gui.sh [--minimized] [其它 GUI 参数原样透传]
#
# 启动前自检 (针对部署后实际出现过的故障模式):
#   1. 套件三件套齐备, 缺失即报错退出;
#   2. bin/linux-wallpaperengine 必须是指向 ../engine/ 的符号链接:
#      引擎 RUNPATH 为 $ORIGIN, 若链接被替换成实体拷贝 (文件管理器
#      复制/解压工具解引用都会造成), 动态库解析失败, 引擎 exit 127
#      秒退, 表现为壁纸不渲染 —— 这里自动重建链接;
#   3. 同一套件已有实例在跑时拒绝重复启动: 后端固定绑定 /tmp 下的
#      socket 且启动时先删除旧 socket 文件, 多实例会互相失联,
#      表现为窗口永远出不来且无任何报错。
set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

die() { echo "run-gui: $*" >&2; exit 1; }

# ---- 1. 布局自检 ----
[ -x "$DIR/gui/linux-wallpaperengine-gui" ] || die "套件不完整: 缺 gui/linux-wallpaperengine-gui"
[ -x "$DIR/engine/linux-wallpaperengine" ] || die "套件不完整: 缺 engine/linux-wallpaperengine"
[ -f "$DIR/lib/libpeony-alpha.so" ] || die "套件不完整: 缺 lib/libpeony-alpha.so"

# ---- 2. bin 入口自愈: 引擎 $ORIGIN 依赖符号链接解析到 engine/ ----
mkdir -p "$DIR/bin"
ENTRY="$DIR/bin/linux-wallpaperengine"
if [ -e "$ENTRY" ] && [ ! -L "$ENTRY" ]; then
	echo "run-gui: bin/linux-wallpaperengine 是实体文件而非符号链接, 已自动修复" >&2
	rm -f "$ENTRY"
fi
ln -sfn ../engine/linux-wallpaperengine "$ENTRY"

# ---- 3. 单实例保护 ----
if pgrep -f "$DIR/gui/linux-wallpaperengine-gui" >/dev/null 2>&1 ||
	pgrep -f "$DIR/gui/resources/linux-wallpaperengine-gui" >/dev/null 2>&1; then
	die "本套件已有实例在运行 (窗口可能收在托盘), 请先退出再启动:
  pkill -f \"$DIR\""
fi

exec env PATH="$DIR/bin:$PATH" "$DIR/gui/linux-wallpaperengine-gui" "$@"
