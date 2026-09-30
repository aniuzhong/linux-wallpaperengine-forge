# engine 定制说明 (ubuntu/26.04 分支)

本目录的 .patch 文件按序套用在"上游钉住版本 (`ENGINE_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `ENGINE_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine diff` 查看当前差异, 对照下方说明
把上游改动合并进对应补丁。

## 0002 — X11 桌面层窗口 (核心, 唯一默认补丁)

把引擎的 GLFW 窗口经 X11 属性提升为桌面层窗口:
`_NET_WM_WINDOW_TYPE_DESKTOP` + `_NET_WM_STATE_BELOW` + `InputHint=False`,
合成器由此把它压到所有窗口之下, 引擎壁纸即桌面背景。

GNOME (mutter) 不支持 wlr-layer-shell, 上游 Wayland 后端无法落点; X11
root-pixmap 路径又被 shell 自绘背景盖住 —— 本补丁是 GNOME 上实现桌面集成
的唯一手段。已实测 Ubuntu 26.04 / GNOME 50 (mutter 将 X11 桌面窗口固定
分配在 BOTTOM 渲染层); 图标层共存由套件的 wallpaper-sink 扩展仲裁
(源码见 src/wallpaper-sink/)。

涉及 `GLFWOpenGLDriver.cpp/.h`、`X11Output.cpp`、`VideoDriver.h`。
