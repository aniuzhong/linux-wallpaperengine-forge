# engine 共享补丁池

本目录的 .patch 按语义 ID 命名, 是跨片共享的补丁池; 每片的套用集合与
顺序由该片 `patches/engine.list` 声明 (一行一个 ID, 按序套用), 由
lib.sh 的 `apply_patch_list` 执行。补丁本身是纯差异、不带注释 —— 含义
与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

条目标题后的【】标注当前引用该片。三片钉住同一上游版本 (`ENGINE_REF`,
见 lib.sh), 同一补丁跨片上下文一致, 迁移/共享只需对目标片 dry-run 一轮。

## 平台集成

### x11-desktop-window 【kylin/v10, ubuntu/26.04】

把引擎的 GLFW 窗口经 X11 属性提升为桌面层窗口:
`_NET_WM_WINDOW_TYPE_DESKTOP` + `_NET_WM_STATE_BELOW` + `InputHint=False`,
合成器由此把它压到所有窗口之下, 引擎壁纸即桌面背景。

平台差异由合成器行为承载, 补丁文本统一:

- kylin/v10: 合成器为 ukui-kwin;
- ubuntu/26.04: 合成器为 GNOME mutter —— 不支持 wlr-layer-shell, 上游
  Wayland 后端无法落点; X11 root-pixmap 路径又被 shell 自绘背景盖住,
  本补丁是 GNOME 上实现桌面集成的唯一手段。已实测 GNOME 50 (mutter 将
  X11 桌面窗口固定分配在 BOTTOM 渲染层); 图标层共存由套件的
  wallpaper-sink 扩展仲裁 (源码见 ubuntu/26.04/src/wallpaper-sink/)。

`X11Output` 同步适配桌面窗口模式, root-pixmap 路径整体退场:
`updateRender()` 置空 (无根位图可上传)、`haveImageBuffer()` 返回 false
(窗口直绘, 无 CPU 回读)、`renderVFlip()` 返回 true (窗口直绘与 window
模式同向; 旧根位图路径靠 CPU 回读自底向上 + XPutImage 自顶向下相抵消
才无需翻转)、`free()` 对 image/pixmap/gc 空值安全 (三者仅根位图模式
存在)。涉及 `GLFWOpenGLDriver.cpp/.h`、`X11Output.cpp`、
`VideoDriver.h`。

### wayland-ready-socket 【kylin/v11】

引擎 layer surface 首次收到 configure 时, 向环境变量 `LWE_READY_SOCKET`
指向的抽象 unixgram socket 发一个 `READY=1\n` 数据报 (sd_notify 风格,
best-effort: env 未设即静默跳过, 独立 CLI 运行零影响), 并保留一行
`[lwe] wayland output mapped` stderr 诊断。GUI 后端
(pkg/background.WatchEngine) 创建该 socket 并经进程环境注入 (上游
processManager 不设 cmd.Env, 子进程继承后端环境), 收到数据报后执行
`Reorder()` 重启 peony, 使桌面图标层抬回引擎上方 (同层内后映射者居上)。
相比旧版日志标记方案: 类型化事件、无文本解析、无 ingest→log→ingest
回路的结构风险、日志开关不影响触发。涉及 `WaylandOutputViewport.cpp`。

实现要点 (首版曾栽在这里): 抽象 socket 的内核查找按 addrlen 全长比较
名称, Go 侧绑定的地址是"零前缀 + 名字"的精确长度; 发送端若按
`sizeof(sockaddr_un)` 传零填充地址, 内核视为不同名称, `sendto` 静默
`ECONNREFUSED`, 数据报永远不可达 (Go↔Go 单测发现不了这种跨语言坑)。
故发送端 addrlen 必须是 `offsetof(sun_path) + 1 + strlen(name)` 的
精确长度, 已按此实现并经真机 C↔Go 往返验证。

**契约耦合**: 与 gui 池的 background-contract 成对 —— 引擎侧发送 /
GUI 侧 (pkg/background) 接收, 缺一边即静默退化。v10/ubuntu 引擎走 X11
驱动, 本补丁为死代码, 不供给。

## 构建兼容

### gcc10-cxx20-compat 【kylin/v10】

麒麟 V10 SP1 自带 gcc-10 (10.3), 其 libstdc++ 原生支持上游使用的全部
ranges/views 设施, `-std=c++20` 旗标也可原样识别, 因此前述代码在
ranges 用法上零修改 (编译器统一钉在 gcc-10, 见 lib.sh)。本补丁只剩
与编译器版本相关的少量修正:

- `CMakeLists.txt`: 摘掉 CEF 缺省旗标里 clang 专属的
  `-Wno-undefined-var-template` (gcc 不认识);
- `CMakeModules/FindFFMPEG.cmake`: 删除空的 `REQUIRED_VARS` 的坏
  `find_package_handle_standard_args` 调用 (cmake 配置期即报错);
- `ColorBuilder.cpp`: libstdc++ 到 13 才有 `<format>`, 以 sstream +
  snprintf 等价实现 CSS 色值展开;
- `MediaSource.h/.cpp`: 显式补 `#include <memory>` —— 上游靠新版
  libstdc++ 的传递包含拿到 `shared_ptr`/`make_unique`, gcc-10 下不再
  传递可得 (`<ranges>` 保留, 本文件确实在用)。

### optional-wayland 【kylin/v10】

wayland 开发库缺失时仅构建 X11 后端 (麒麟交付目标本就是 X11)。涉及
`CMakeLists.txt`。v11 恰恰依赖 wayland 驱动, 必须不供给本补丁。

### wayland-118-compat 【kylin/v10】

`wl_output` 的 name/description 事件仅存在于 libwayland ≥ 1.20; 宿主机
(麒麟 V10 带 1.18) 直接构建在 `WaylandOutputViewport.cpp` 的监听器
初始化处编译失败。按 `WAYLAND_VERSION_NUMBER` 分支, 并对低版本下不再
引用的回调加 `[[maybe_unused]]`。容器构建 (无 wayland dev) 不受影响。

## 渲染保真

### scene-native-resolution 【kylin/v10, kylin/v11, ubuntu/26.04】

场景 FBO 按显示器原生分辨率渲染, 修正高分屏上的模糊 (X11 与
Wayland 驱动同路径生效)。涉及 `CWallpaper.h`、`CScene.cpp/.h`。

### no-anisotropy-on-nearest 【kylin/v10, kylin/v11, ubuntu/26.04】

上游 `CTexture::setupOpenGLParameters` 对所有纹理无条件设置
`GL_TEXTURE_MAX_ANISOTROPY = 8.0`, 包括带 NoInterpolation 标志的
像素画纹理。探针程序实测 (GL 4.3 compat, Mesa 26.0.8 SVGA3D,
2×2 四色纹理放大 256× 后读回): NEAREST 单独设置时纯 4 色忠实还原;
叠加各向异性 8.0 后变成 486 色 (74% 像素被混合) —— SVGA3D 的
各向异性路径把 NEAREST 放大采样降级为混合采样; NVIDIA
(Quadro RTX 4000) 则尊重 MAG_FILTER=NEAREST 不受各向异性影响。
这正是同一引擎、同一壁纸 (Aesthetic City, 480×300 像素画,
纹理 flags=7) 在 v10 真机锐利、两台 VMware 虚机模糊的直接原因。

修复: 各向异性只在线性过滤分支设置。NoInterpolation 是素材作者的
最近邻意图标记, 各向异性对它本无意义; 修复对缺陷驱动免疫, 在合规
驱动上与原行为逐像素等价, 三片统一供给、无任何 GPU 探测分支。

判别方法论 (可复用): 对截图统计等值相邻像素对占比与唯一色数 ——
最近邻放大 ~75-95% (色数=源调色板), 线性混合 60-74% (色数数千)。

涉及 `Render/CTexture.cpp: setupOpenGLParameters()`。

## 缺陷修复

### varying-injection 【kylin/v10, kylin/v11, ubuntu/26.04】

部分创意工坊壁纸 (旧版效果着色器) 的 frag 使用了只在配套 vert 里
声明的 varying —— Windows WE 按整个 program 转译/链接, 容忍这种跨阶段
裸引用; 本引擎用 glslang 按阶段单独校验, frag 校验失败后 `toGlsl`
返回空源码, 真实 GL 驱动编译空串抛异常, 携带该特效的对象整体放弃
初始化, 只剩场景清屏色。确定性触发, 与 GPU/驱动无关。

典型后果: the among forest (2172956777) 的旧版 foliagesway frag 使用
`v_Bounds` 但未声明 (vert 声明), 全屏背景层渲染失败, 画面只剩
`clearcolor` 0.7 灰。实测给 frag 补一行声明后同一引擎完整渲染。

修复: `GLSLContext::toGlsl` 在 glslang 校验前, 从注释剥离后的 vert
组装文本收集 varying 声明 (跳过 `#define varying out` 等预处理器行、
跨预处理行的伪声明), 对 frag 中按整词使用、又未声明的, 把声明插入
`void main` 所在行之前 —— 此时 `#define varying in` 宏已生效, 注入的
`varying` 与壁纸自带声明走同一展开路径。只增声明不改语义。

已知取舍: varying 依赖的 uniform (如 `g_Bounds`) 引擎不接线时保持
GL 默认值 0, 派生值可能为 inf/NaN, 但最终乘以极小振幅后为亚像素
位移, 肉眼不可见; 如需精确接线应作为独立问题回馈上游。

涉及 `Render/Shaders/GLSLContext.cpp: toGlsl()` (新增辅助:
stripComments/declaredVaryings/injectMissingVaryings)。

### tolerant-parsers 【kylin/v10, kylin/v11, ubuntu/26.04】

解析器对畸形/缺失字段的总体容错, 三处修复合一 (2026-10-10 由原
tolerant-parsers 与 text-padding-numeric 两补丁合并, 树哈希证明与
原两补丁依序套用等价):

- `ObjectParser::parse`: particle/light/shape 段存在但为 null 时不再
  进入对应解析器抛异常, 按普通对象降级;
- `WallpaperParser::parseScene`: `orthogonalprojection` 缺失时取默认
  值 (`auto: true`), 不再 require 失败终止整个场景;
- `ObjectParser::parseText`: padding 字段仅数字时取值, 其余类型
  (含 WE 编辑器标准写法 vec2 字符串 `"67.00000 67.00000"`) 安全置 0
  —— 上游按 `optional<int>` 解析, nlohmann 对字符串取 int 直接抛
  `type_error.302`, 无捕获直达 `std::terminate`, 引擎在 GL 初始化前
  abort, 桌面黑屏。确定性触发, 实测 Dark Mode Win XP Bliss
  (3218303536) 的 Time/Date 两个文本对象命中。渲染器目前不消费
  padding (CText 注明 "Phase 2"), 无功能损失; 将来实现文本 padding
  时应换用真正的 vec2 解析。

涉及 `Data/Parsers/ObjectParser.cpp`、
`Data/Parsers/WallpaperParser.cpp`。

### color4-alpha-fold 【kylin/v10, kylin/v11, ubuntu/26.04】

`genericimage2/3` 等 VERSION 2+ 材质着色器只通过 `g_Color4.a` 接收对象
透明度 (`color = texSample2D(...) * g_Color4`, 着色器内不引用
`g_Alpha`/`g_UserAlpha`), 但引擎把 `g_Color4` 直接接线为对象颜色
(默认白色不透明), 对象的 alpha 属性从未生效。后果: 任何带
`{"alpha": {"user": ..., "value": 0.0}}` 的 VERSION 2 对象以完全不
透明渲染 —— 典型如 Blue Sky [4K] 的全屏 "black" 调光遮罩 (alpha 默认
0) 把整个画面盖成黑屏。修复: 接线 `g_Color4` 时把对象的 alpha 折入
w 分量 (`color4.a * getAlpha()`)。默认 alpha 为 1, 不影响其他壁纸;
仅使用 `g_Color`/`g_UserAlpha` 的 VERSION 1 材质不读 `g_Color4`,
同样不受影响。确定性缺陷, 与 GPU/驱动无关 (已在 SVGA3D 与 Kylin V10
真机验证同因)。

涉及 `Render/Objects/Effects/CPass.cpp: setupRenderReferenceUniforms()`
(g_Color4 接线处)。

### conditional-visibility-inverse 【kylin/v10, kylin/v11, ubuntu/26.04】

场景 JSON 里对象/特效的 `visible` 可以绑定用户属性加条件:
`{"user": {"name": X, "condition": V}, "value": B}`。WE 的语义是
"show/hide when" —— 属性值匹配 V 时取 B, **不匹配时取 !B** (编辑器
UI 对应 "show when / hide when" 两种绑定)。上游实现丢失了反向
分支: 求值固定为 `(属性值 == V)`, B 被忽略, 导致所有 `value: false`
的条件设置 ("hide when" 绑定) 在不匹配时错误地保持隐藏。

典型后果: Blue Sky [4K] (2944773634) 的水面反射 blur 特效绑定的是
`{"condition": "1", "value": false}` ("White Line=Hide 时隐藏", 即
默认 Show 状态下应显示), 上游求值使其默认不可见 —— Linux 上倒影
缺少 Windows 默认就有的高斯模糊, 且与白线的共存关系被破坏。修复:
attachCondition 时快照内联值, 求值改为 `匹配 ? B : !B`; 属性缺失时
仍回退内联值, 行为不变。多分支 combo (每个选项一个条件绑定) 语义
随之自然成立。

涉及 `Data/Model/DynamicValue.cpp/.h`。

✔ **已知问题 (已由 passthrough-declared-size 关闭)**: 本条目曾记录
"blur 启用后其合成溢出到全屏, 白线上方的天空/风车被轻度柔化"。
2026-10 定位: 溢出不在 blur 渲染链, 而是 Water (composelayer) 对象
矩形被绑定纹理 (_rt_FullFrameBuffer, 全屏尺寸) 覆盖了场景声明的带状
尺寸 —— 见 passthrough-declared-size。修复后复测: Water 的
skip-object 帧差贡献区从屏幕行 ~202 起收缩到 454 起 (白线在 453),
线上方 blur A/B 信号归零, 锐度差异回到运行间噪声基线内。

### passthrough-declared-size 【kylin/v10, kylin/v11, ubuntu/26.04】

composelayer/projectlayer 类 passthrough 对象的布局尺寸被绑定纹理
覆盖: 逐帧 `updateGeometryBuffers → getSize()` 在 `m_texture` 填充后
返回纹理像素尺寸 —— 而 composelayer 绑定的正是全屏
`_rt_FullFrameBuffer` (3840×2160), 对象矩形从场景声明的带状尺寸膨胀
为"origin 居中的全屏"。带内特效 (无 mask 的 blur/waterripple/
cursorripple) 随之作用并写回整个膨胀区。确定性触发, 与 GPU/驱动无关。

典型后果: Blue Sky [4K] (2944773634) 的 Water 带 (3840×360) 膨胀为
全屏, 白线之上的云/风车被模糊柔化 (skip-object 帧差: 贡献区从屏幕
行 ~253 起)。Windows WE 按声明尺寸布局, 模糊仅存在于白线之下。

修复: `resolveGeometrySize` 对 passthrough 模型改用场景声明的
`image.size` (声明为 0 时仍回退纹理尺寸)。构造路径本就先于
`detectTexture` 取声明尺寸, 行为不变; 仅逐帧路径被纠正。实测
skip-object 帧差贡献区收缩到白线之下 (454 起, 原 202 起; 白线在
453), blur 专项 A/B 线上方信号归零、线下方保留; Blue Sky 全景目视
云缘锐利。projectlayer 类全屏对象声明尺寸即全屏, 行为不变。

涉及 `Render/Objects/CImage.cpp: resolveGeometrySize()`。

### default-uvs-center-crop 【kylin/v10, kylin/v11, ubuntu/26.04】

纵横比失配壁纸在 default 缩放模式下产生越界 UV: 上游
`WallpaperState::updateTextureUVs<DefaultUVs>` 按视口/投影的横竖
取向 (而非纵横比对比) 决定裁切轴, 对"横屏视口 + 更宽投影"(32:9
双联壁纸 among forest, 3840×1080, 于 16:9 屏) 只调 updateVs, 算出
v ∈ [-0.5, 1.5] —— 越出 [0,1] 的两段被 CLAMP_TO_EDGE 拖成上下两条
模糊带 (与 scene-native-resolution 无关, 上游 FBO=设计分辨率时同样
发生); 竖版壁纸在横屏下同理产生 u ∈ [-2.7, 3.7] 的左右拖影。
fill 模式 (ZoomFillUVs) 的 max 缩放 + 居中裁切才是正确语义, Windows
WE 的 auto 行为即如此。

修复: 删除 DefaultUVs 专用实现, switch 中 DefaultUVs 贯穿到
ZoomFillUVs, default 与 fill 同语义。纵横比匹配的壁纸两种模式都得
到 [0,1], 行为逐位不变; 失配壁纸从拖影带变为居中裁切。实验闭环:
同一台 16:9 机器 default 复现上下拖影带、fill 正常居中裁切, 切换
仅动 config 的 scaling 字段。

涉及 `Render/WallpaperState.cpp`。

### x11-fullscreen-windowid-fix 【kylin/v10, ubuntu/26.04】

`X11FullScreenDetector::anythingFullscreen()` 把 `GLFWwindow*` 指针
强转成 X11 窗口 ID 传给 `XQueryTree`, 每帧 BadWindow 失败后提前
return: 全屏检测永远返回 false, 且第一次查询分配的根窗口 children
数组 (~2.4KB) 永不释放 —— 4K60 实测泄漏 ~145KB/s, 5 天累积 73GB 直至
OOM。修复: 经 `glfwGetX11Window` 取真实窗口 ID, 两处错误路径补
`XFree(children)`, 修正 `schildren` 分支释放错缓冲 (UAF + 双重释放)
的问题。判定条件同步收紧: 跳过 `_NET_WM_STATE_BELOW` (桌面壳层, 如
peony), 且要求 `_NET_WM_STATE_FULLSCREEN` 原子 —— UKUI 屏保常驻全屏
几何的可视对话框没有该原子, 纯几何匹配会永久误暂停壁纸。涉及
`X11FullScreenDetector.cpp`。

检测器按显示驱动注册 ("x11"/"wayland" 各一), 引擎运行时按活跃驱动
选择: v10 与 ubuntu (Xwayland, X11 驱动) 命中本补丁; kylin/v11 走
Wayland 检测器, 本补丁为死代码, 不供给。

## 文件重叠说明

- `CMakeLists.txt` 同时承载 gcc10-cxx20-compat (编译旗标) 与
  optional-wayland (Wayland 可选), hunks 位于不同区段, 按序套用互不
  干扰;
- `ObjectParser.cpp` 的容错已由 tolerant-parsers 单补丁统一承载
  (2026-10-10 合并, 见历史备注)。
