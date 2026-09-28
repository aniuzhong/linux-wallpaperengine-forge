# gui 定制说明

本目录的 .patch 文件按序套用在"上游钉住版本 (`GUI_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `GUI_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C src/linux-wallpaperengine-gui diff` 查看当前差异, 对照下方说明
把上游改动合并进对应补丁。

注意: `src/backend/go.mod` 的 pkg/peony 接线**不属于任何补丁** —— 它由
build-gui.sh 的 `go mod edit` 在每次构建时现场注入 (apply_patches 每轮
会把 go.mod 重置回上游)。

## 逻辑补丁清单

### 001 — Workshop 移入 utilityProcess

Valve 的 breakpad 崩溃钩子只应存在于独立进程。新增
`steamworksWorker.ts`: steamworks.js 运行在 Electron utilityProcess
worker 里, 经一套 JSON RPC 协议 (`init`/`call` → `ready`/`result`) 与主
进程通信, 工坊代码无论怎么崩都不波及 UI, 父进程按需重启并优雅降级;
`workshopService.ts` 改造为经 worker 代理, `vite.config.ts` 随动。

### 002 — 桌面透明注入接线

`app.go` 三处接线: 导入 pkg/peony、启动时
`SetBridge(logger.Println)` + `go Attach()` (后台注入 UKUI 桌面壳,
失败经日志桥浮出不阻断启动)、`Cleanup()` 里 `Detach()` 还原。
注入逻辑全部在 forge 自有模块 `pkg/peony`, 这里只有接线。

### 003 — 日志历史回放

`logger` 增加一个 500 条的有界环形历史, `Subscribe()` 订阅时先回放再
直播: 日志页晚于后端启动打开时, 也能看到启动期的日志 (应用初始化、
壁纸启动、`[inject]` 注入流), 而不是只剩订阅之后的行。
