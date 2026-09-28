# Wallpaper Engine 壁纸套件 (LWE Forge)

在麒麟 V10 SP1 / UKUI (X11) 桌面上运行 Steam 创意工坊的动态壁纸。
整个套件就是一个目录:GUI、渲染引擎、桌面注入库开箱即用——解压即可运行,删除目录即卸载。

## 环境要求

- 麒麟 V10 SP1 (glibc 2.31) 或更新的 64 位 Linux,X11 会话
- Wallpaper Engine 的 `assets` 素材目录(安装 Steam 版 Wallpaper Engine 时自带)。
  常见位置:`~/.steam/steam/steamapps/common/wallpaper_engine/assets`;
  Flatpak 版 Steam 在 `~/.var/app/com.valvesoftware.Steam/...`。首次启动后在
  GUI 设置里指向它即可
- 浏览与订阅创意工坊壁纸需要 Steam 客户端处于运行状态;已下载的壁纸离线可用

## 快速开始

```bash
tar xzf lwe-forge-*.tar.gz
cd lwe-forge-*/
./run-gui.sh
```

首次启动后在 GUI 里选一张壁纸即可;之后每次启动会自动恢复上次的壁纸。

## 目录结构

```
gui/        GUI 本体 (Electron + Go 后端, 含 Workshop 功能)
engine/     linux-wallpaperengine 引擎 (含 CEF 运行时)
lib/        桌面透明注入库 (peony-alpha, GUI 后端自动定位)
bin/        包内引擎入口 (run-gui.sh 通过 PATH 前置使用)
run-gui.sh  启动入口
VERSION     成分表 (组件版本与补丁清单)
```

## 日常使用

- 托盘右键:显示/隐藏主窗口、退出(退出即停止壁纸)
- 最小化到托盘:窗口关闭,壁纸继续运行(由留守的后端进程维持)
- 升级:退出 GUI,用新套件覆盖本目录,再启动即可

## 移除

- 先退出 GUI(托盘右键退出,桌面会自动还原),再删除整个套件目录
- 若直接删除了仍在运行的套件,桌面可能保持黑屏无图标;重新解压并运行一次
  GUI,或手动执行 `peony-qt-desktop -w -d` 即可恢复

## 行为说明与已知边界

- 桌面透明集成:GUI 启动时自动注入 UKUI 桌面壳(peony-qt-desktop),使其背景
  变为透明,引擎壁纸从桌面图标层下方透出;GUI 退出时自动还原桌面
- 暂停或停止壁纸期间,桌面显示为素色;恢复播放或退出 GUI 后回到原生壁纸
- 注入状态下在系统设置里更换壁纸会暂时失去透明效果,重启 GUI 即恢复
- 桌面壳若崩溃或退出,UKUI 不会自动重生它,桌面会保持黑屏且无图标;重新启动
  GUI 会自动拉起桌面壳并恢复壁纸

## 排查

- 注入与还原的完整日志:`~/.local/share/lwe-forge/peony-alpha.log`
- GUI 运行日志:主窗口的"日志"页
