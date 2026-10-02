# engine 定制说明 (ubuntu/26.04 分支)

本目录的 .patch 文件按序套用在"上游钉住版本 (`ENGINE_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `ENGINE_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine diff` 查看当前差异, 对照下方说明
把上游改动合并进对应补丁。

## 补丁分层

- **必装** (`patches/engine/*.patch`): 修复缺陷或恢复 WE 语义, 任何
  环境都需要; 每次构建自动按文件名序套用。
- **可选** (`patches/engine/optional/*.patch`): 有代价的权衡或实验性,
  不自动套用; 把文件名 (一行一个, `#` 为注释) 写进
  `optional/enabled` 即启用, 套用顺序 = enabled 行序, 追加在必装
  全集之后。可选补丁以"必装后的树"为基准维护。

编号即套用顺序。提交历史中的旧编号映射 (均指重排前的旧文件, 与现行
同号补丁无关): 0002→0001、0005→0002、0006→0003、0004 不变。

---

## 必装补丁

### 0001 — X11 桌面层窗口 【核心·平台集成】

把引擎的 GLFW 窗口经 X11 属性提升为桌面层窗口:
`_NET_WM_WINDOW_TYPE_DESKTOP` + `_NET_WM_STATE_BELOW` + `InputHint=False`,
合成器由此把它压到所有窗口之下, 引擎壁纸即桌面背景。

GNOME (mutter) 不支持 wlr-layer-shell, 上游 Wayland 后端无法落点; X11
root-pixmap 路径又被 shell 自绘背景盖住 —— 本补丁是 GNOME 上实现桌面集成
的唯一手段。已实测 Ubuntu 26.04 / GNOME 50 (mutter 将 X11 桌面窗口固定
分配在 BOTTOM 渲染层); 图标层共存由套件的 wallpaper-sink 扩展仲裁
(源码见 src/wallpaper-sink/)。

X11Output 同步切换渲染语义（root-pixmap 路径整体退场）：
`updateRender()` 置空（无根位图可上传）、`haveImageBuffer()` 返回
false（窗口直绘，无 CPU 回读）、`renderVFlip()` 返回 true（窗口直绘
与 window 模式同向；旧根位图路径靠 CPU 回读自底向上 + XPutImage 自顶
向下相抵消才无需翻转）；`free()` 对 image/pixmap/gc 空值安全（三者仅
根位图模式存在）。

涉及 `GLFWOpenGLDriver.cpp/.h: promoteToDesktopWindow()`、
`X11Output.cpp: initX11Background()/updateRender()/free()/renderVFlip()/haveImageBuffer()`、
`VideoDriver.h: promoteToDesktopWindow()`。

### 0002 — VERSION 2 材质的对象 alpha 折入 g_Color4 【核心·缺陷修复】

`genericimage2/3` 等 VERSION 2+ 材质着色器只通过 `g_Color4.a` 接收对象
透明度（`color = texSample2D(...) * g_Color4`，着色器内不引用
`g_Alpha`/`g_UserAlpha`），但引擎把 `g_Color4` 直接接线为对象颜色
（默认白色不透明），对象的 alpha 属性从未生效。后果：任何带
`{"alpha": {"user": ..., "value": 0.0}}` 的 VERSION 2 对象以完全不
透明渲染——典型如 Blue Sky [4K] 的全屏 "black" 调光遮罩（alpha 默认
0）把整个画面盖成黑屏。修复：接线 `g_Color4` 时把对象的 alpha 折入
w 分量（`color4.a * getAlpha()`）。默认 alpha 为 1，不影响其他壁纸；
仅使用 `g_Color`/`g_UserAlpha` 的 VERSION 1 材质不读 `g_Color4`，
同样不受影响。确定性缺陷，与 GPU/驱动无关（已在 SVGA3D 与 Kylin V10
真机验证同因）。

涉及 `Render/Objects/Effects/CPass.cpp: setupRenderReferenceUniforms()`（g_Color4 接线处）。

### 0003 — 条件可见性的 show/hide-when 语义 【核心·缺陷修复】

场景 JSON 里对象/特效的 `visible` 可以绑定用户属性加条件：
`{"user": {"name": X, "condition": V}, "value": B}`。WE 的语义是
"show/hide when"——属性值匹配 V 时取 B，**不匹配时取 !B**（编辑器
UI 对应 "show when / hide when" 两种绑定）。上游实现丢失了反向
分支：求值固定为 `(属性值 == V)`，B 被忽略，导致所有 `value: false`
的条件设置（"hide when" 绑定）在不匹配时错误地保持隐藏。

典型后果：Blue Sky [4K] (2944773634) 的水面反射 blur 特效绑定的是
`{"condition": "1", "value": false}`（"White Line=Hide 时隐藏"，即
默认 Show 状态下应显示），上游求值使其默认不可见——Linux 上倒影
缺少 Windows 默认就有的高斯模糊，且与白线的共存关系被破坏。修复：
attachCondition 时快照内联值，求值改为 `匹配 ? B : !B`；属性缺失时
仍回退内联值，行为不变。多分支 combo（每个选项一个条件绑定）语义
随之自然成立。

涉及 `Data/Model/DynamicValue.cpp/.h`。

✔ **已知问题（已由 0007 关闭）**：本条目曾记录"blur 启用后其合成溢出
到全屏，白线上方的天空/风车被轻度柔化"。2026-10 定位：溢出不在 blur
渲染链，而是 Water（composelayer）对象矩形被绑定纹理
（_rt_FullFrameBuffer，全屏尺寸）覆盖了场景声明的带状尺寸——见 0007。
修复后复测：Water 的 skip-object 帧差贡献区从屏幕行 ~202 起收缩到
454 起（白线在 453），线上方 blur A/B 信号归零，锐度差异回到运行间
噪声基线内。

### 0004 — NoInterpolation 壁纸的最近邻上屏 【增强·保真】

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

涉及 `CWallpaper.cpp/.h: setupShaders()/setUpscaleNearest()/render()`、
`Wallpapers/CScene.cpp/.h: 构造/detectNearestUpscale()`。

### 0005 — 文本对象 padding 的类型宽容解析 【核心·缺陷修复】

WE 编辑器给文本对象写 padding 的标准格式是 vec2 字符串
（`"padding": "67.00000 67.00000"`），上游 `ObjectParser::parseText` 却
按 `optional<int>` 解析，nlohmann 对字符串取 int 直接抛
`type_error.302`，异常沿 `loadBackground` 无捕获直达 `std::terminate`——
引擎在场景解析阶段（GL 初始化之前）abort，桌面无壁纸呈现为黑屏。
确定性触发，与 GPU/驱动无关；实测 Dark Mode Win XP Bliss
(3218303536) 的 Time/Date 两个文本对象命中（该壁纸黑屏的根因），
扫描本机全部 13 个已下载壁纸仅此一例。

修复：仅当字段为数字时取值，其余类型（含 vec2 字符串）安全置 0。
渲染器目前不消费 padding（CText 注明 "Phase 2"），故无功能损失、
无行为变化；将来实现文本 padding 时应换用真正的 vec2 解析。

涉及 `Data/Parsers/ObjectParser.cpp: parseText()`。

### 0006 — 片元着色器缺失 varying 声明的自动补全 【核心·缺陷修复】

部分创意工坊壁纸（旧版效果着色器）的 frag 使用了只在配套 vert 里
声明的 varying——Windows WE 按整个 program 转译/链接，容忍这种跨阶段
裸引用；本引擎用 glslang 按阶段单独校验，frag 校验失败后 `toGlsl`
返回空源码，真实 GL 驱动编译空串抛异常，携带该特效的对象整体放弃
初始化，只剩场景清屏色。确定性触发，与 GPU/驱动无关。

典型后果：the among forest (2172956777) 的旧版 foliagesway frag 使用
`v_Bounds` 但未声明（vert 声明），全屏背景层渲染失败，画面只剩
`clearcolor` 0.7 灰。实测给 frag 补一行声明后同一引擎完整渲染。

修复：`GLSLContext::toGlsl` 在 glslang 校验前，从注释剥离后的 vert
组装文本收集 varying 声明（跳过 `#define varying out` 等预处理器行、
跨预处理行的伪声明），对 frag 中按整词使用、又未声明的，把声明插入
`void main` 所在行之前——此时 `#define varying in` 宏已生效，注入的
`varying` 与壁纸自带声明走同一展开路径。只增声明不改语义；四个已
验证壁纸（among forest、Bliss、Blue Sky、RDR2）解析零失败，
截图像素统计与各自基准一致。

已知取舍：varying 依赖的 uniform（如 `g_Bounds`）引擎不接线时保持
GL 默认值 0，派生值可能为 inf/NaN，但最终乘以极小振幅后为亚像素
位移，肉眼不可见；如需精确接线应作为独立问题回馈上游。

涉及 `Render/Shaders/GLSLContext.cpp: toGlsl()`（新增辅助：stripComments/declaredVaryings/injectMissingVaryings）。

### 0007 — passthrough 对象使用场景声明尺寸 【核心·缺陷修复】

composelayer/projectlayer 类 passthrough 对象的布局尺寸被绑定纹理
覆盖：逐帧 `updateGeometryBuffers → getSize()` 在 `m_texture` 填充后
返回纹理像素尺寸——而 composelayer 绑定的正是全屏 `_rt_FullFrameBuffer`
(3840×2160)，对象矩形从场景声明的带状尺寸膨胀为"origin 居中的全屏"。
带内特效（无 mask 的 blur/waterripple/cursorripple）随之作用并写回
整个膨胀区。确定性触发，与 GPU/驱动无关。

典型后果：Blue Sky [4K] (2944773634) 的 Water 带 (3840×360) 膨胀为
全屏，白线之上的云/风车被模糊柔化（skip-object 帧差：贡献区从屏幕
行 ~253 起）。Windows WE 按声明尺寸布局，模糊仅存在于白线之下。

修复：`resolveGeometrySize` 对 passthrough 模型改用场景声明的
`image.size`（声明为 0 时仍回退纹理尺寸）。构造路径本就先于
`detectTexture` 取声明尺寸，行为不变；仅逐帧路径被纠正。实测
skip-object 帧差贡献区收缩到白线之下（454 起，原 202 起；白线在 453），
blur
专项 A/B 线上方信号归零、线下方保留；Blue Sky 全景目视云缘锐利。
projectlayer 类全屏对象声明尺寸即全屏，行为不变。

涉及 `Render/Objects/CImage.cpp: resolveGeometrySize()`。

---

## 可选补丁 (optional/)

启用方法：把文件名写入 `optional/enabled`（一行一个，行序即套用序）。

### native-resolution — 场景按输出分辨率渲染 【增强·权衡】（未迁移）

CScene 覆写 `setupFramebuffers`，把场景 FBO 建在输出全尺寸而非壁纸
设计分辨率，blit 变 1:1，全屏清晰度与 Windows WE 一致；代价是 GPU
填充率按面积比上升（4K 屏上 3840×2160 场景 ≈ 4×）。对水彩/照片类
画风收益有限，对细节型壁纸收益明显。旧版补丁见 git 历史
(`patches/engine/0003-scene-native-resolution.patch`, 已删)，需以当前
必装全集为基准重写后迁入。
