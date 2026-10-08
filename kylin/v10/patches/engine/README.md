# engine 定制说明

本目录的 .patch 文件按序套用在"上游钉住版本 (`ENGINE_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `ENGINE_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine diff` 查看当前差异, 对照下方说明把
上游改动合并进对应补丁。

## 逻辑补丁清单

### 0001 — gcc10 / c++20 兼容

麒麟 V10 SP1 自带 gcc-10 (10.3),其 libstdc++ 原生支持上游使用的全部
ranges/views 设施,`-std=c++20` 旗标也可原样识别,因此上游代码在
ranges 用法上零修改(编译器统一钉在 gcc-10,见 lib.sh)。本补丁只剩
与编译器版本无关的少量修正:

- `CMakeLists.txt`:摘掉 CEF 缺省旗标里 clang 专属的
  `-Wno-undefined-var-template`(gcc 不认识);
- `CMakeModules/FindFFMPEG.cmake`:删除空的 `REQUIRED_VARS` 的坏
  `find_package_handle_standard_args` 调用(cmake 配置期即报错);
- `ColorBuilder.cpp`:libstdc++ 到 13 才有 `<format>`,以 sstream +
  snprintf 等价实现 CSS 色值展开;
- `MediaSource.h/.cpp`:显式补 `#include <memory>` —— 上游靠新版
  libstdc++ 的传递包含拿到 `shared_ptr`/`make_unique`,gcc-10 下不再
  传递可得(`<ranges>` 保留,本文件确实在用)。

### 0002 — X11 桌面层窗口 (核心)

把引擎的 GLFW 窗口经 X11 属性提升为桌面层窗口:
`_NET_WM_WINDOW_TYPE_DESKTOP` + `_NET_WM_STATE_BELOW` + `InputHint=False`,
合成器 (ukui-kwin 等) 由此把它压到所有窗口之下, 引擎壁纸即桌面背景;
`X11Output` 同步适配桌面窗口模式 (资源释放与垂直翻转修正), 替代旧的
root-pixmap 路径。涉及 `GLFWOpenGLDriver.cpp/.h`、`X11Output.cpp`、
`VideoDriver.h`。

### 0003 — 场景原生分辨率

场景 FBO 按显示器原生分辨率渲染, 修正高分屏上的模糊。涉及
`CWallpaper.h`、`CScene.cpp/.h`。

### 0004 — 片段着色器 varying 注入

创意工坊着色器常在片段端直接使用顶点 varying 而不声明; 编译前自动注入
缺失的声明, 让这批壁纸可以正常编译。涉及 `ShaderUnit.cpp/.h`。

### 0005 — 解析器容错

`project.json` / object 字段缺失或类型不符时降级处理, 不再抛异常终止
整个引擎。涉及 `ObjectParser.cpp`、`WallpaperParser.cpp`。

### 0006 — Wayland 可选

wayland 开发库缺失时仅构建 X11 后端 (麒麟交付目标本就是 X11)。
涉及 `CMakeLists.txt`。

### 0007 — X11 全屏检测窗口 ID 修复 (内存泄漏根因)

`X11FullScreenDetector::anythingFullscreen()` 把 `GLFWwindow*` 指针强转成
X11 窗口 ID 传给 `XQueryTree`, 每帧 BadWindow 失败后提前 return: 全屏检测
永远返回 false, 且第一次查询分配的根窗口 children 数组 (~2.4KB) 永不释放
—— 4K60 实测泄漏 ~145KB/s, 5 天累积 73GB 直至 OOM。修复: 经
`glfwGetX11Window` 取真实窗口 ID, 两处错误路径补 `XFree(children)`, 修正
`schildren` 分支释放错缓冲 (UAF + 双重释放) 的问题。判定条件同步收紧:
跳过 `_NET_WM_STATE_BELOW` (桌面壳层, 如 peony), 且要求
`_NET_WM_STATE_FULLSCREEN` 原子 —— UKUI 屏保常驻全屏几何的可视对话框
没有该原子, 纯几何匹配会永久误暂停壁纸。涉及 `X11FullScreenDetector.cpp`。

### 0008 — Wayland 1.18 宿主机构建兼容

`wl_output` 的 name/description 事件仅存在于 libwayland ≥ 1.20; 宿主机
(麒麟 V10 带 1.18) 直接构建在 `WaylandOutputViewport.cpp` 的监听器
初始化处编译失败。按 `WAYLAND_VERSION_NUMBER` 分支, 并对低版本下不再
引用的回调加 `[[maybe_unused]]`。容器构建 (无 wayland dev) 不受影响。
涉及 `WaylandOutputViewport.cpp`。

## 文件重叠说明

`CMakeLists.txt` 同时承载 0001 (编译旗标) 与 0006 (Wayland 可选) 两项
修改, hunks 位于不同区段, 按序套用互不干扰。其余文件与补丁一一对应。
