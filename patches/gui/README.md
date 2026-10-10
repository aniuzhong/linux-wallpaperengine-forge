# gui 共享补丁池

本目录的 .patch 按语义 ID 命名, 是跨片共享的补丁池; 每片的套用集合与
顺序由该片 `patches/gui.list` 声明 (一行一个 ID, 按序套用), 由
lib.sh 的 `apply_patch_list` 执行。补丁本身是纯差异、不带注释 —— 含义
与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

注意: `src/backend/go.mod` 的 forge Go 模块接线 (pkg/peony /
pkg/background)**不属于任何补丁** —— 它由 build-gui.sh 的 `go mod
edit` 在每次构建时现场注入, 归属片由 target.sh 的 GO_REPLACE_PKG/DIR
声明 (apply_patch_list 每轮会把 go.mod 重置回上游)。

条目标题后的【】标注当前引用该片。三片钉住同一上游版本 (`GUI_REF`,
见 lib.sh)。

## 集成

### peony-inject-wiring 【kylin/v10】

`app.go` 三处接线: 导入 pkg/peony、启动时
`SetBridge(logger.Println)` + `go Attach()` (后台注入 UKUI 桌面壳,
失败经日志桥浮出不阻断启动)、`Cleanup()` 里 `Detach()` 还原。
注入逻辑全部在 forge 自有模块 `pkg/peony`, 这里只有接线。
平台策略: X11/UKUI (与 v11 的 background-contract 互为替代策略,
两者都在 app.go 相同锚点插入, 不可同时选中)。

### background-contract 【kylin/v11】

`app.go` 四处接线: `SetDebugLogf`(诊断桥)、启动时 `Prepare()`
(备份用户壁纸三元组 → gsettings 指向全透明 PNG → peony 重绘, 失败经
日志浮出不阻断启动)、`go WatchEngine(...)` (引擎代际监视, 收到
engine 池 wayland-ready-socket 的就绪数据报后 reorder 桌面层序)、
`Cleanup()` 里 `Detach()` 还原; 另将默认 `Layer` 从 `bottom` 改为
`background`。契约逻辑全部在 forge 自有模块 `pkg/background`, 这里
只有接线。平台策略: Wayland/UKUI。

## 缺陷修复

### wallpaper-process-hardening 【kylin/v10, kylin/v11, ubuntu/26.04】

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
打开失败静默放弃): 引擎卡死时内存历史随后端退出丢失, 文件留现场。
此前引擎输出不落盘, 排障时后端日志里看不到引擎"临终遗言"。

2026-10-10 起三片统一供给 —— 上游泄漏与平台无关, 两个 Wayland 片
同样每切一次壁纸漏一个引擎实例, 供给缺口已关闭。另注意上游
`KillAll()` 末尾的 `killall -e linux-wallpaperengine` 实际按 15 字符
截断名匹配, 会把 Electron/后端一起带走 (详见 notes 坑 28), 仅显式
kill-all 路径触发, 正常切换不走该行。

### ipc-encoder-mutex 【kylin/v10, kylin/v11, ubuntu/26.04】

同一条 socket 连接上有两处并发写同一个 `json.Encoder`: 读循环的应答
(`HandleIPC`/`HandleSystem`) 与 outCh goroutine 的事件广播 (log 等)。
`Encoder` 非并发安全, 交织产物是前端/客户端无法解析的损坏 JSON 行,
切壁纸时的日志风暴即可诱发。修复: server 侧以 `lockedEncoder`
(互斥锁包装) 统一所有写出; `HandleIPC`/`HandleSystem` 的参数从具体
`*json.Encoder` 放宽为 `handlers.ResponseWriter` 接口, 由 server 注入
并发安全实现, 处理器侧零改动。

2026-10-09 复现实验: 在 v10 上摘除本补丁后以 ping 风暴 + apply 日志
风暴 + 820KB 大响应同连接竞争约 4.5 万行, 未复现字节级损坏 (小消息
单 write 原子, 现代内核对 unix 流写按 skb 串行化); 原始触发环境为
Steam 工坊大库的 UI 流量。2026-10-10 起三片统一供给。保留理由: Go
内存模型意义下的真实数据竞争, 修复为零成本手术, 大库环境成立时必现。

