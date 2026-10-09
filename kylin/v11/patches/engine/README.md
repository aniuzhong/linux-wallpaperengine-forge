# engine 定制说明 (kylin/v11 片)

本目录的 .patch 文件按序套用在"上游钉住版本 (`ENGINE_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `ENGINE_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine diff` 查看当前差异, 对照下方说明
把上游改动合并进对应补丁。

## 补丁清单

### 001 — Wayland 映射标记 (桌面层序时序)

引擎 layer surface 首次收到 configure 时向 stderr 打一行
`[lwe] wayland output mapped`, 作为"表面已进入合成器场景"的确定性信号;
GUI 后端 (pkg/background.WatchEngine) 经日志流捕获该行后执行 `Reorder()`
重启 peony, 使桌面图标层抬回引擎上方 (同层内后映射者居上)。涉及
`WaylandOutputViewport.cpp`。

## 历史备注

原 0001-v11-min-compat (gcc-12/libstdc++12 基线修正: MediaSource 传递
包含缺失、ColorBuilder 的 <format> 改写、ProjectParser 的裸 tolower
ranges 改写) 已于片编译器切换 gcc-13 时整条撤除 —— libstdc++ 13 原生
提供 <format> 且传递包含自足, 无补丁上游在 gcc-13 下全量构建通过
(无 0001 上游 + 001 本补丁, 实测)。
