# Wallpaper Engine 壁纸套件 (LWE Forge, ubuntu/26.04)

在 Ubuntu 26.04 / GNOME 50 (Wayland) 桌面上运行 Steam 创意工坊的动态壁纸。
整个套件就是一个目录:GUI、渲染引擎、桌面集成扩展开箱即用——解压即可运行,
删除目录即卸载。

## 环境要求

- Ubuntu 26.04 GNOME Wayland 会话 (64 位)
- Wallpaper Engine 的 `assets` 素材目录(安装 Steam 版 Wallpaper Engine 时自带,
  snap 版 Steam 在 `~/snap/steam/common/.local/share/Steam/steamapps/common/
  wallpaper_engine/assets`)。首次启动后在 GUI 设置里指向它即可
- 浏览与订阅创意工坊壁纸需要 Steam 客户端处于运行状态;已下载的壁纸离线可用

## 快速开始

```bash
tar xzf lwe-forge-*.tar.gz
cd lwe-forge-*/
./run-gui.sh
```

注意:请解压到家目录或 /opt 等常规位置;`/tmp` 通常挂载为 nosuid,解在那里
GUI 沙箱无法启用(run-gui.sh 会自动降级运行并提示)。

首次运行会自动安装 GNOME 桌面集成扩展 (wallpaper-sink)。**首次需要注销并
重新登录一次**让扩展生效,run-gui.sh 会给出提示;之后每次启动自动恢复上次
的壁纸。

## 目录结构

```
gui/              GUI 本体 (Electron + Go 后端, 上游未打补丁)
engine/           linux-wallpaperengine 引擎 (含 CEF 运行时, 已裁剪)
bin/              引擎入口包装器 (Wayland 会话环境修正)
gnome-extension/  wallpaper-sink 扩展源 (run-gui.sh 自动安装)
run-gui.sh        启动入口
VERSION           成分表 (组件版本与补丁清单)
```

## 桌面集成原理 (GNOME 50 Wayland)

GNOME 50 下没有现成的动态壁纸挂点,本套件用三个部件拼出集成:

1. **引擎 (0002 补丁)** 把自身窗口提升为 X11 桌面层窗口
   (`_NET_WM_WINDOW_TYPE_DESKTOP`),经 Xwayland 渲染。mutter 50 将 X11
   桌面窗口固定分配在 `BOTTOM` 渲染层;
2. **wallpaper-sink 扩展** 把 DING 桌面图标窗口 (Wayland, 占用更低的
   `DESKTOP` 层) 改写进 `NORMAL` 层——否则图标永远在壁纸之下;
3. **bin/ 包装器** 在 GUI 拉起引擎时修正环境 (`XDG_SESSION_TYPE=x11`、
   屏蔽 `WAYLAND_DISPLAY`),因为 mutter 不支持引擎 Wayland 驱动所依赖的
   wlr-layer-shell。

最终栈序:静态背景 < 引擎壁纸 (BOTTOM) < 桌面图标 (NORMAL 层底) < 普通窗口。

## 日常使用

- 托盘右键:显示/隐藏主窗口、退出(退出即停止壁纸)
- 最小化到托盘:窗口关闭,壁纸继续运行
- 升级:退出 GUI,用新套件覆盖本目录,再启动即可

## 移除

- 先退出 GUI,再删除整个套件目录;扩展残留在
  `~/.local/share/gnome-shell/extensions/wallpaper-sink@lwe-forge`,可手动删除

## 已知边界与排查

- 托盘在但 GUI 窗口不出现（点托盘 Show 也没反应）:典型原因是 `gui/chrome-sandbox`
  权限无法自愈(无 sudo 免密、或解压在 nosuid 分区)，后端二次拉起的 Electron 被
  SUID 沙箱自检 FATAL 杀掉且崩溃日志被丢弃。run-gui.sh 检测到不可修复时会导出
  `ELECTRON_DISABLE_SANDBOX=1` 兜底(经环境继承覆盖二次拉起)；也可手工
  `sudo chown root:root gui/chrome-sandbox && sudo chmod 4755 gui/chrome-sandbox`
  恢复完整沙箱。切勿用 sudo 运行本套件
- 缩放模式: 上游 GUI 的默认缩放 `default` 在壁纸与屏幕宽高比不一致时
  (16:9 壁纸 on 16:10 屏为最常见组合) 会把采样 UV 越出纹理边缘, 配合
  clamp 模式 `clamp` 在屏幕上下各拉出一条约 80px 的边缘拉伸带 (任意壁纸
  皆然)。run-gui.sh 首次启动时会把该默认值一次性迁移为 `fill` (等比铺满,
  左右各裁约 5%), 之后 GUI 里的任何手动选择都不再被改动; 想看完整画面
  可选 `fit` + clamp 模式 `border` (上下留黑边)
- web 类型壁纸依赖 CEF,无 GPU 加速的虚拟机 (如 VMware SVGA3D 软渲染) 下
  可能触发 V8 沙箱崩溃 (SIGTRAP);scene 与 video 类型不受影响
- 引擎手动调试命令 (可绕过 GUI):
  ```bash
  env XDG_SESSION_TYPE=x11 DISPLAY=:0 WAYLAND_DISPLAY=no-such-socket \
      engine/linux-wallpaperengine -r <显示器名> <壁纸路径或工坊ID> \
      --assets-dir <WE assets 目录>
  ```
- 扩展诊断日志:journalctl --user | grep wallpaper-sink
- 若图标被壁纸遮住:确认扩展处于 ACTIVE
  (`gnome-extensions info wallpaper-sink@lwe-forge`),必要时注销重登
