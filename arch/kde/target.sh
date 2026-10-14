#
# arch/kde 平台画像 — 纯声明, 无逻辑; 被 lib.sh source。
# 宿主: Arch Linux (rolling) + KDE Plasma (Wayland); 与 kylin 切片共享语义补丁池。
#

CC="${CC:-gcc}"
CXX="${CXX:-g++}"

ENGINE_PATCH_LIST="$TARGET_DIR/patches/engine.list"
GUI_PATCH_LIST="$TARGET_DIR/patches/gui.list"

# 无 GO_REPLACE_PKG — 不接 Kylin background 契约模块
# 无 INTEGRATION_BUILD — KDE 桌面集成走 systemd user unit (后补)
# 无 OVERLAY_DIR
