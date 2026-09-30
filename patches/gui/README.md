# gui 定制说明

本目录的 .patch 文件按序套用在"上游钉住版本 (`GUI_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `GUI_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine-gui diff` 查看当前差异, 对照下方说明
把上游改动合并进对应补丁。

注意: `src/backend/go.mod` 的 pkg/background 接线**不属于任何补丁** —— 它
由 build-gui.sh 的 `go mod edit` 在每次构建时现场注入 (apply_patches 每轮
会把 go.mod 重置回上游)。

## 逻辑补丁清单

### 002 — V11 壁纸契约接线 (pkg/background)

`app.go` 接线: 导入 pkg/background; 启动时 `Prepare()` (备份壁纸指针并
指向全透明 PNG, 刷新 peony 重绘; 失败经日志浮出不阻断启动)、
`SetDebugLogf` + `logger.Subscribe()` 转发协程 (引擎表面映射标记随日志
行喂给 `IngestLine`)、`go WatchEngine()` (标记到达后 `Reorder()` 重启
peony, 图标层抬回引擎上方); `Cleanup()` 里 `Detach()` 还原用户壁纸。
契约逻辑全部在 forge 自有模块 `pkg/background`, 这里只有接线。另将
config.go 默认 `Layer` 从 `bottom` 改为 `background`。

