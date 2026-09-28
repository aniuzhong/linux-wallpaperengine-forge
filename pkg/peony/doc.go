// Package peony 向 UKUI 桌面壳 (peony-qt-desktop) 注入 libpeony-alpha.so
// (LD_PRELOAD interposer, 源码见 src/peony-qt-desktop), 使其背景变为真
// 透明, 让下层的引擎桌面层壁纸透出来。
//
// 契约 (均经真机验证):
//   - 不碰壁纸指针: shim 的匹配列表喂 gsettings 当前值, Detach 零残留;
//   - 幂等: 已注入的桌面壳原样保留, 干净运行的桌面壳不打扰;
//   - 环境卫生: 注入变量只走 exec.Cmd.Env, 绝不 os.Setenv —— 兄弟子进程
//     (引擎) 永远拿到干净环境;
//   - 优雅失败: 死掉的桌面壳回到普通桌面 (用户原生壁纸), 本包不做状态
//     收敛 (无 watcher)。
//
// 日志: shim 原始行 append 至 ~/.local/share/lwe-forge/peony-alpha.log
// (真相源); EnableRelay 加 "[inject] " 前缀转发到 SetBridge 装的日志桥。
package peony

// shimPathEnv 允许显式指定 shim 库路径, 开发与非常规布局的出口。
const shimPathEnv = "LWE_PEONY_SHIM"
