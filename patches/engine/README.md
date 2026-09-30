# engine 定制说明 (kylin/v11/x64)

本目录的 .patch 文件按序套用在"上游钉住版本 (`ENGINE_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `ENGINE_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine diff` 查看当前差异, 对照下方
说明把上游改动合并进对应补丁。

## 逻辑补丁清单

### 0001 — gcc12 缺包含修复 (最小兼容)

上游钉住版本按 gcc9 时代头文件传递包含的习惯书写, gcc12 的 libstdc++
更精简导致: `MediaSource.h` 缺 `<optional>`/`<memory>` (连带 MediaCover.cpp
报 "no member url"), `MediaSource.cpp` 缺 `<memory>`; `ColorBuilder.cpp`
使用 `<format>` (gcc13 才有), 改写为 `<sstream>`; 另将两处 ranges 算法
(range replace/transform + 裸 tolower) 改写为普通 transform/replace。
涉及 `MediaSource.h/.cpp`、`ColorBuilder.cpp`、`ProjectParser.cpp`。

验证状态: 构建+运行已验证 (v11 实机, 2026-09)。

### 0002 — Wayland 映射标记 (桌面层序时序)

引擎 layer surface 首次收到 configure 时向 stderr 打一行
`[lwe] wayland output mapped`, 作为"表面已进入合成器场景"的确定性信号;
GUI 后端 (pkg/background.WatchEngine) 经日志流捕获该行后执行 `Reorder()`
重启 peony, 使桌面图标层抬回引擎上方 (同层内后映射者居上)。涉及
`WaylandOutputViewport.cpp`。
