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

## 0004 — NoInterpolation 壁纸的最近邻上屏

壁纸根纹理带 `NoInterpolation` 标志（像素风素材的作者标记）且覆盖整个
场景时，上屏 blit 着色器把 UV 吸附到源纹素中心（`floor(uv*ts)+0.5)/ts`），
等效最近邻放大，像素风壁纸铺满 HiDPI 屏保持硬边；默认仍为普通采样。

问题分三层，归属不同：

1. 功能缺失（上游）：场景 FBO 按壁纸设计分辨率创建（如 480×300），
   `NoInterpolation` 只在 CImage 的场景内采样生效，FBO → 屏幕的最终
   blit 无条件线性过滤，像素风壁纸拉伸到 HiDPI 必然发虚（Windows WE
   呈现硬边）。这是上游没实现的功能，不是驱动相关行为。
2. 采样器缺陷（部署环境，第三方）：VMware SVGA3D 忽略纹理首次使用
   之后的 `glTexParameteri` 修改。实测：blit 前 `glGetTexParameteriv`
   读回 GL_NEAREST，但同帧把 FBO 分别按 NEAREST/BILINEAR 放大与引擎
   实际输出比对，与 BILINEAR 的 RMS 仅 0.14（NEAREST 为 5.72）——
   采样仍是线性。这违反 GL 规范，属 Mesa svga / VMware 层，值得单独
   上报；不是上游或本套件代码的问题。
3. 实现选型（本套件）：因此过滤不落在采样器状态上，而是着色器内
   UV 吸附。该公式就是 GL 规范对 NEAREST 的定义，合规驱动上与
   `glTexParameteri` 逐像素等价，缺陷驱动上唯一可行；运行时探测不可行
   （SVGA3D 对默认帧缓冲的 glReadPixels 亦不可靠，曾读回全黑），
   renderer 字符串匹配是打地鼠，故无条件启用、仅对命中的壁纸生效。
   代价：blit 每次 frag 多数条 ALU，可忽略。

检测在场景构造完成时做（此时对象纹理已加载）：覆盖全场景的
NoInterpolation 图层触发；混搭场景与小物件不启用。已知取舍：跨帧
crossfade 的渐变纹素同样被吸附放大（与 WE 的 spritesheetrefreshsync
行为一致）。若上游将来改为按输出分辨率渲染场景（blit 变 1:1），本
补丁自动退化为无害路径。

涉及 `CWallpaper.cpp/.h`、`Wallpapers/CScene.cpp/.h`。
