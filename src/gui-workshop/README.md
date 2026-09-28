# src/gui-workshop — Workshop utilityProcess 隔离

forge 自有的 GUI 前端源码（TypeScript/Electron 主进程），构建时由
`build-gui.sh` 整体覆盖到 GUI 源码树对应路径（overlay 模式），
**不经过补丁**。与 Go 侧 `pkg/peony`（go.mod replace 接入）同一教义：
逻辑进自有源码，补丁只留接线（001 补丁仅剩 `vite.config.ts` 的
worker 入口行）。

## 文件与落位

| 本目录文件 | 安装到 GUI 源码树 |
|---|---|
| `steamworksWorker.ts` | `src/frontend/main/services/steamworksWorker.ts` |
| `workshopService.ts` | `src/frontend/main/services/workshopService.ts`（覆盖上游文件） |

## 内容

- **steamworksWorker.ts** — Electron utilityProcess worker 入口。
  steamworks.js（Valve steam_api）只在本进程初始化：其 breakpad
  崩溃钩子无论怎么崩都不波及 UI 进程；与父进程走 JSON RPC
  （`init`/`call` → `ready`/`result`）。
- **workshopService.ts** — 覆盖上游同名模块，改为经 worker 代理的
  RPC 客户端（`WorkshopWorker`）；worker 死亡时优雅降级，
  `get-subscribed-items` 回退到磁盘扫描，主页列表不依赖 Steam 在线。

## 上游演进告警（UPSTREAM_BASE）

覆盖式安装会遮蔽上游对同路径文件的改动。`UPSTREAM_BASE` 记录
overlay 所基于的上游 blob hash（`git rev-parse GUI_REF:<path>`），
`build-gui.sh` 每次构建时比对钉住版本：不一致则打警告（不阻断），
提示对照 `git -C third_party/linux-wallpaperengine-gui show <ref>:<path>`
把上游改动合并进 overlay。上游新增文件（无基线行）不设告警。

## 修改约定

改动在本目录的真实源码上进行；`tsc --noEmit` 门禁（build-gui.sh）
在构建期拦截未定义标识符等类型错误。禁止手改 001 补丁来携带逻辑。
