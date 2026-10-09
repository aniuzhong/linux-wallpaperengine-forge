# Wallpaper Engine 壁纸套件 (LWE Forge)

在麒麟 V11 (kylin-wlcom/Wayland) 桌面上运行 Steam 创意工坊的动态壁纸。
整个套件就是一个目录:GUI、渲染引擎开箱即用——解压即可运行,删除目录即卸载。
桌面集成 (壁纸契约) 通过 gsettings 公开配置完成, 无任何注入。

## 环境要求

- 麒麟 V11 (kylin-wlcom, Wayland 会话) 的 64 位 Linux
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
- 若直接删除了仍在运行的套件,壁纸指针仍指向透明图,桌面会显示素色;重新
  解压并运行一次 GUI,或在系统设置里任选一张壁纸即可恢复

## 行为说明与已知边界

- 桌面透明集成(壁纸契约):GUI 启动时备份系统壁纸指针并指向一张全透明
  PNG,peony 画透明背景,引擎壁纸从桌面图标层下方透出;引擎表面映射完成后
  自动重启一次桌面壳,让图标层保持在壁纸上方(图标闪烁数秒属预期);GUI
  退出时还原原壁纸
- 暂停或停止壁纸期间,桌面显示为素色;恢复播放或退出 GUI 后回到原生壁纸
- 接管状态下在系统设置里更换壁纸会暂时失去透明效果,重启 GUI 即恢复
- 桌面壳若崩溃或退出,会话看门狗会自动重生它;重生后的桌面壳仍位于壁纸
  上方,壁纸不受影响

## 排查

- 壁纸接管数据:`~/.local/share/lwe-forge/`(transparent.png 与原壁纸备份
  background-state.json,请勿手工删除)
- GUI 运行日志:主窗口的"日志"页(含 [background] 接管与层序事件)
