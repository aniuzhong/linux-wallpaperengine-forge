# engine 定制说明 (kylin/v11 片)

本目录的 .patch 文件按序套用在"上游钉住版本 (`ENGINE_REF`, 见 lib.sh)"
的克隆上 (由 lib.sh 的 `apply_patches` 完成)。补丁本身是纯差异、不带
注释 —— 含义与缘由统一放在本文件, 补丁内容变更时请同步更新对应条目。

换 `ENGINE_REF` 时补丁可能因上下文变化套用失败 (显性报错): 用
`git -C third_party/linux-wallpaperengine diff` 查看当前差异, 对照下方说明
把上游改动合并进对应补丁。

## 补丁清单

### 001 — Wayland 就绪 socket (桌面层序时序)

引擎 layer surface 首次收到 configure 时,向环境变量 `LWE_READY_SOCKET`
指向的抽象 unixgram socket 发一个 `READY=1\n` 数据报 (sd_notify 风格,
best-effort:env 未设即静默跳过,独立 CLI 运行零影响),并保留一行
`[lwe] wayland output mapped` stderr 诊断。GUI 后端
(pkg/background.WatchEngine) 创建该 socket 并经进程环境注入
(上游 processManager 不设 cmd.Env,子进程继承后端环境),收到数据报
后执行 `Reorder()` 重启 peony,使桌面图标层抬回引擎上方 (同层内后映射
者居上)。相比旧版日志标记方案:类型化事件、无文本解析、无
ingest→log→ingest 回路的结构风险、日志开关不影响触发。涉及
`WaylandOutputViewport.cpp`。

实现要点 (首版曾栽在这里): 抽象 socket 的内核查找按 addrlen 全长比较
名称, Go 侧绑定的地址是"零前缀 + 名字"的精确长度; 发送端若按
`sizeof(sockaddr_un)` 传零填充地址, 内核视为不同名称, `sendto` 静默
`ECONNREFUSED`, 数据报永远不可达 (Go↔Go 单测发现不了这种跨语言坑)。
故发送端 addrlen 必须是 `offsetof(sun_path) + 1 + strlen(name)` 的
精确长度, 已按此实现并经真机 C↔Go 往返验证。

## 历史备注

原 0001-v11-min-compat (gcc-12/libstdc++12 基线修正: MediaSource 传递
包含缺失、ColorBuilder 的 <format> 改写、ProjectParser 的裸 tolower
ranges 改写) 已于片编译器切换 gcc-13 时整条撤除 —— libstdc++ 13 原生
提供 <format> 且传递包含自足, 无补丁上游在 gcc-13 下全量构建通过
(无 0001 上游 + 001 本补丁, 实测)。
