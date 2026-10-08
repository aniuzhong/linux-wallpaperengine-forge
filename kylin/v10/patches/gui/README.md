# gui 定制说明

> 本片仅存 001/002。003/004/005 已提升为跨平台公共补丁, 移至
> `patches/gui/`, 条目移至 `patches/README.md`（下文 003/004/005
> 三节保留作机制说明）。

本目录的 .patch 文件按序套用在"上游钉住版本 (`GUI_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `GUI_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine-gui diff` 查看当前差异, 对照下方说明
把上游改动合并进对应补丁。

注意: `src/backend/go.mod` 的 pkg/peony 接线**不属于任何补丁** —— 它由
build-gui.sh 的 `go mod edit` 在每次构建时现场注入 (apply_patches 每轮
会把 go.mod 重置回上游)。

## 逻辑补丁清单

### 001 — Workshop 移入 utilityProcess (仅 vite 接线)

Valve 的 breakpad 崩溃钩子只应存在于独立进程。本补丁只剩
`vite.config.ts` 一处: main 入口扩为 `[main.ts, steamworksWorker.ts]`
双入口。实现全部在 forge 自有源码 `src/gui-workshop/`
(`steamworksWorker.ts` worker 进程 + `workshopService.ts` 覆盖上游,
经 JSON RPC `init`/`call` → `ready`/`result` 通信, 工坊崩溃不波及 UI,
worker 死亡时优雅降级), 由 build-gui.sh 构建期覆盖安装, 上游演进经
`UPSTREAM_BASE` hash 告警。历史教训: 早期版本以 700+ 行补丁形态携带
全部逻辑, 手工改 diff 产生过 7 个静默缺陷 (未定义类名/常量/方法),
esbuild 不做类型检查全部放行 —— 这正是该逻辑迁出自补丁的原因。

### 002 — 桌面透明注入接线

`app.go` 三处接线: 导入 pkg/peony、启动时
`SetBridge(logger.Println)` + `go Attach()` (后台注入 UKUI 桌面壳,
失败经日志桥浮出不阻断启动)、`Cleanup()` 里 `Detach()` 还原。
注入逻辑全部在 forge 自有模块 `pkg/peony`, 这里只有接线。

### 003 — 日志历史回放

`logger` 增加一个 500 条的有界环形历史, `Subscribe()` 订阅时先回放再
直播: 日志页晚于后端启动打开时, 也能看到启动期的日志 (应用初始化、
壁纸启动、`[inject]` 注入流), 而不是只剩订阅之后的行。

### 004 — 壁纸引擎终止硬化 + 引擎日志落盘

背景是一次真实故障: GPU 图形栈卡死后引擎主循环阻塞在自动重启的
阻塞调用里, 捕获 SIGTERM 置的退出标志永远轮不到检查, 旧版
`killWallpaperInternal` 只发一次 SIGTERM、不等退出、不升级 ——
每切一次壁纸泄漏一个引擎实例, 一夜堆到 8 个 (单屏 4K 窗口 + GL
上下文), 加速显存耗尽。`manager.go` 两处修复:

- **三段式终止**: SIGTERM → 等 3s 宽限 → SIGKILL → 确认退出。判活靠
  `ActiveWallpaper.done` (收尸 goroutine `Wait` 后关闭, 先关再拿锁,
  避免与持锁的终止方互等), 不二次调 `Wait` (全进程只许一个调用者)。
- **孤儿清理**: `UpdateWallpapers` 对账前扫 `/proc`, 对本后端未认领的
  `linux-wallpaperengine` (argv[0] basename 精确匹配) 补 SIGKILL,
  上一代后端遗留的实例不再堆积。

`logger.go` 的 `WallpaperLog` 同步把引擎控制台输出落盘到
`/tmp/lwe-forge-engine-<screen>.log` (按屏幕分文件, 后端每次启动截断;
打开失败静默放弃): 引擎卡死时内存环形历史随后端退出丢失, 文件留
现场。此前引擎输出不落盘, 排障时后端日志里看不到引擎"临终遗言"。

### 005 — IPC encoder 并发竞态

同一条 socket 连接上有两处并发写同一个 `json.Encoder`: 读循环的应答
(`HandleIPC`/`HandleSystem`) 与 outCh goroutine 的事件广播 (log 等)。
`Encoder` 非并发安全, 交织产物是前端/客户端无法解析的损坏 JSON 行,
切壁纸时的日志风暴即可诱发。修复: server 侧以 `lockedEncoder`
(互斥锁包装) 统一所有写出; `HandleIPC`/`HandleSystem` 的参数从具体
`*json.Encoder` 放宽为 `handlers.ResponseWriter` 接口, 由 server 注入
并发安全实现, 处理器侧零改动。

