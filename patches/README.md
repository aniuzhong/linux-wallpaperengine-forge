# patches/ — 公共补丁

所有 target 均套用的跨平台补丁；套用序 = 公共层在前、平台片在后
（由各片 `target.sh` 的 `*_PATCH_DIRS` 声明）。含义与缘由按目录记账，
补丁内容变更时同步更新对应条目。

## gui/

- `003-logs-history-replay` — 后端日志 500 条环形历史，`Subscribe()` 建
  订阅先回放历史再进直播（日志页晚开也能看到启动期日志）
- `004-wallpaper-process-hardening` — 引擎三段式终止（SIGTERM → 等 3s →
  SIGKILL + 确认）、扫 /proc 补杀孤儿引擎实例、引擎控制台输出按屏落盘
- `005-ipc-encoder-race` — 同一 socket 上应答与事件广播并发写
  `json.Encoder` 的竞态，统一为互斥锁包装的 `lockedEncoder`

平台片内补丁的账本见各片目录（如 `kylin/v10/patches/gui/README.md`、
`kylin/v10/patches/engine/README.md`）。
