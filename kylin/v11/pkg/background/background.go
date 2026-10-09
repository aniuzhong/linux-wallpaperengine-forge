// Package background —— 麒麟 V11 (kylin-wlcom + peony 4.21) 的壁纸契约。
//
// V11 桌面形态: peony-qt-desktop 以 wlr-layer-shell background 层绘制壁纸与
// 图标; 引擎同样挂 background 层, 同层内后映射者居上。方案:
//
//	Prepare : 备份用户壁纸三元组 → gsettings 指向全透明 PNG → peony 重绘
//	Reorder : 重启 peony 进程 —— 表面重建后居后映射位置, 图标层抬回引擎上方
//	          (-u 只重绘背景内容, 不重建 Wayland 表面, 做不到层序修正)
//	Detach  : 还原用户壁纸
//
// 零注入: 与 peony 的全部交互是 gsettings 键与命令行, 均为公开契约。
// 引擎时序由 WatchEngine 驱动 (引擎出现 → 就绪 socket 数据报 → Reorder),
// 对 GUI 上游代码零侵入。
package background

import (
	"encoding/json"
	"errors"
	"fmt"
	"maps"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// transparentPNG 是 64x64 全透明 RGBA PNG (96 字节, 预生成)。
const transparentPNG = "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x40\x00\x00\x00\x40\x08\x06\x00\x00\x00\xaa\x69\x71\xde\x00\x00\x00\x27\x49\x44\x41\x54\x78\x9c\xed\xc1\x01\x0d\x00\x00\x00\xc2\xa0\xf7\x4f\x6d\x0e\x37\xa0\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x77\x03\x40\x40\x00\x01\x8f\xf2\xc9\x51\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82"

// State 是 Detach 所需的用户壁纸原值。
type State struct {
	PictureFilename string `json:"picture-filename"`
	DrawBackground  string `json:"draw-background"`
	PictureOpacity  string `json:"picture-opacity"`
}

// 状态文件: 崩溃后下次启动仍可还原; 存在即视为"已接管"。
func statePath() string {
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "lwe-forge", "background-state.json")
}

func dataDir() string {
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "lwe-forge")
}

// TransparentPNGPath 返回透明壁纸的落盘路径 (Prepare 保证存在)。
func TransparentPNGPath() string {
	return filepath.Join(dataDir(), "transparent.png")
}

func gsettings(schema, key string) (string, error) {
	out, err := exec.Command("gsettings", "get", schema, key).Output()
	if err != nil {
		return "", err
	}
	return string(out[:len(out)-1]), nil // 去尾部换行
}

func gsettingsSet(schema, key, value string) error {
	return exec.Command("gsettings", "set", schema, key, value).Run()
}

func refreshPeony() error {
	// peony 未运行时无害 (单实例语义: 命令即刻返回); 会话看门狗退避期间
	// 由调用方自行拉起 peony 后再 Reorder 亦可
	return exec.Command("peony-qt-desktop", "-u").Run()
}

// isActive 报告当前是否处于接管状态 (状态文件存在)。
func isActive() bool {
	_, err := os.Stat(statePath())
	return err == nil
}

// Prepare 幂等接管: 首次备份用户壁纸, 确保透明 PNG 在位, 应用并刷新桌面。
// 同时拉起引擎就绪 socket 并注入环境 (必须在任何引擎拉起之前完成)。
func Prepare() (string, error) {
	startReadySocket()
	if !isActive() {
		filename, err := gsettings("org.mate.background", "picture-filename")
		if err != nil {
			return "", errors.New("read picture-filename: " + err.Error())
		}
		drawBg, _ := gsettings("org.mate.background", "draw-background")
		opacity, _ := gsettings("org.mate.background", "picture-opacity")
		state := State{PictureFilename: filename, DrawBackground: drawBg, PictureOpacity: opacity}
		b, _ := json.MarshalIndent(state, "", "  ")
		if err := os.MkdirAll(filepath.Dir(statePath()), 0o755); err != nil {
			return "", err
		}
		if err := os.WriteFile(statePath(), b, 0o644); err != nil {
			return "", err
		}
	}
	if err := os.MkdirAll(dataDir(), 0o755); err != nil {
		return "", err
	}
	png := []byte(transparentPNG)
	if err := os.WriteFile(TransparentPNGPath(), png, 0o644); err != nil {
		return "", err
	}
	if err := gsettingsSet("org.mate.background", "picture-filename", TransparentPNGPath()); err != nil {
		return "", err
	}
	if err := gsettingsSet("org.mate.background", "draw-background", "true"); err != nil {
		return "", err
	}
	if err := refreshPeony(); err != nil {
		return "", errors.New("peony refresh: " + err.Error())
	}
	return TransparentPNGPath(), nil
}

// Detach 还原用户壁纸并清理状态 (幂等; 未接管时无事发生)。
func Detach() error {
	b, err := os.ReadFile(statePath())
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var state State
	if err := json.Unmarshal(b, &state); err != nil {
		return err
	}
	if state.PictureFilename != "" {
		_ = gsettingsSet("org.mate.background", "picture-filename", state.PictureFilename)
	}
	if state.DrawBackground != "" {
		_ = gsettingsSet("org.mate.background", "draw-background", state.DrawBackground)
	}
	if state.PictureOpacity != "" {
		_ = gsettingsSet("org.mate.background", "picture-opacity", state.PictureOpacity)
	}
	os.Remove(statePath())
	return refreshPeony()
}

// Reorder 在引擎 (重)启动后调用: 重建 peony 表面, 图标层抬回引擎上方。
//
// 注意 -u 做不到这件事: 它只让 peony 重绘背景内容, Wayland 表面保持
// 原位, 层序不变。可靠的层序修正 = 重启 peony 进程 (后映射者居上,
// 同 wlcom background 层)。图标会闪烁数秒, 属预期。
func Reorder() error {
	return restartPeony()
}

// ---- peony 生命周期 (V11: 看门狗存在, 重启竞速的两种结局都接受) -------------

const (
	peonyDesktopMarker  = "peony-qt-desktop"
	singleInstanceGlob  = "/tmp/qtsingleapp-peonyq*"
	peonyRespawnTimeout = 8 * time.Second
)

// peonyPids 返回本 uid 的桌面壳进程。
func peonyPids() []int {
	var pids []int
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	selfUID := uint32(os.Getuid())
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		if st, err := os.Stat("/proc/" + entry.Name()); err != nil || st.Sys().(*syscall.Stat_t).Uid != selfUID {
			continue
		}
		cmdline, err := os.ReadFile("/proc/" + entry.Name() + "/cmdline")
		if err != nil {
			continue
		}
		fields := strings.Fields(strings.ReplaceAll(string(cmdline), "\x00", " "))
		if len(fields) > 0 && filepath.Base(fields[0]) == peonyDesktopMarker {
			pid, _ := strconv.Atoi(entry.Name())
			pids = append(pids, pid)
		}
	}
	return pids
}

// clearSingleInstanceLocks 清掉残留的单实例锁, 否则新实例会以为该把
// 位置让给一个已不存在的桌面壳而直接退出。
func clearSingleInstanceLocks() {
	matches, _ := filepath.Glob(singleInstanceGlob)
	for _, m := range matches {
		os.Remove(m)
	}
}

func stopPeony() {
	for _, pid := range peonyPids() {
		_ = syscall.Kill(pid, syscall.SIGTERM)
	}
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if len(peonyPids()) == 0 {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	for _, pid := range peonyPids() {
		_ = syscall.Kill(pid, syscall.SIGKILL)
	}
	time.Sleep(300 * time.Millisecond)
}

// spawnPeony 分离拉起桌面壳 (独立进程组, 脱离 GUI 生命周期)。
func spawnPeony() error {
	cmd := exec.Command("peony-qt-desktop", "-w", "-d")
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	return cmd.Start()
}

// restartPeony 重启桌面壳: 杀旧 → 清锁 → 自己先拉一个; 若 ukui-session
// 看门狗抢先重生了干净实例同样有效 —— 任何 peony 都在后映射位置, 层序即正确。
func restartPeony() error {
	pids := peonyPids()
	if len(pids) > 0 {
		stopPeony()
	}
	clearSingleInstanceLocks()
	if err := spawnPeony(); err != nil {
		return err
	}
	deadline := time.Now().Add(peonyRespawnTimeout)
	for time.Now().Before(deadline) {
		if len(peonyPids()) > 0 {
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	return errors.New("peony did not come back within " + peonyRespawnTimeout.String())
}

// enginePIDs 扫描 /proc, 按 exe 符号链接的基名精确匹配引擎二进制。
// 不能用 pgrep -f: 引擎名是 GUI/后端进程名的前缀 (linux-wallpaperengine-gui),
// 命令行匹配会把 GUI 自己也算成引擎, 导致"引擎从无到有"的跳变永远检测不到,
// 切换壁纸后的 reorder 便永不触发。
func enginePIDs(names []string) map[int]bool {
	pids := make(map[int]bool)
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return pids
	}
	want := make(map[string]bool, len(names))
	for _, n := range names {
		want[n] = true
	}
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		exe, err := os.Readlink("/proc/" + e.Name() + "/exe")
		if err != nil {
			continue
		}
		if want[filepath.Base(exe)] {
			pids[pid] = true
		}
	}
	return pids
}

// ---- 确定性时序: 就绪 socket (sd_notify 风格) ---------------------------------
//
// 引擎 (001-wayland-ready-socket 补丁) 在 layer surface 首次 ack_configure
// 时向 LWE_READY_SOCKET 指向的抽象 unixgram socket 发一个 READY=1 数据报;
// 本包创建 socket 并经 os.Setenv 注入 —— 引擎子进程继承后端环境 (上游
// processManager 不设 cmd.Env), 无需侵入拉起代码。数据报到达 → 有待决
// 代际则投一枚令牌 → WatchEngine 执行 reorder。相比旧版"引擎 stderr 标记
// 行 → logger.Subscribe → 字符串匹配":类型化事件、无文本解析、与日志管线
// 彻底解耦 —— 触发路径上没有 logger, ingest→log→ingest 自激回路在结构上
// 不存在, 日志开关/格式变化不影响触发。
const readyPayload = "READY=1\n"

var (
	tokenMu       sync.Mutex
	tokenWaitChan chan struct{} // 每个引擎代际一个 (容量 1); nil = 当前无待决 reorder
	debugLogf     func(string, ...any)
	readyOnce     sync.Once
)

// SetDebugLogf 装配就绪信号命中的单行诊断出口 (生产由 GUI 侧补丁接线传
// logger.Printf)。仅命中时打点; 出口虽是 logger, 但触发输入已是 socket,
// logger 不再回流本包, 无自激递归的通路。
func SetDebugLogf(f func(string, ...any)) { debugLogf = f }

// startReadySocket 创建抽象 unixgram 就绪 socket 并注入环境, 拉起读取
// 协程; sync.Once 幂等 (Prepare 与 WatchEngine 都会调用, 先到先建)。
// 抽象地址随本进程退出自动消亡, 无文件系统残留; 后端重启即新地址,
// 旧引擎进程残留的失效 env 变量只会发送失败, 无副作用。
func startReadySocket() {
	readyOnce.Do(func() {
		name := fmt.Sprintf("lwe-forge-ready-%d", os.Getpid())
		conn, err := net.ListenPacket("unixgram", "@"+name)
		if err != nil {
			if debugLogf != nil {
				debugLogf("[background] readiness socket unavailable (%v); engine map signal disabled", err)
			}
			return
		}
		os.Setenv("LWE_READY_SOCKET", "@"+name)
		go func() {
			buf := make([]byte, 64)
			for {
				n, _, err := conn.ReadFrom(buf)
				if err != nil {
					return // socket 已关闭
				}
				if n < len(readyPayload) || string(buf[:len(readyPayload)]) != readyPayload {
					continue // 非 READY 数据报 (误投/探测), 丢弃
				}
				deliverToken()
			}
		}()
		if debugLogf != nil {
			debugLogf("[background] engine readiness socket at @%s (env injected)", name)
		}
	})
}

// deliverToken 在有待决代际时投递一枚令牌。容量 1: 同代际多个引擎进程
// 各发一个数据报, reorder 仍只做一次; WatchEngine 的接收侧是 200ms 轮询
// select (带 default, 永不泊车), 无缓冲通道的非阻塞发送永远失败 —— 令牌
// 必丢, reorder 必不触发 (图标被引擎盖住的直接原因), 带缓冲后令牌在此
// 等待下一拍。
func deliverToken() {
	tokenMu.Lock()
	ch := tokenWaitChan
	tokenMu.Unlock()
	if ch == nil {
		return // 无待决代际: 数据报来自已作废的代际, 丢弃
	}
	if debugLogf != nil {
		debugLogf("[background] engine surface-ready signal received, scheduling desktop reorder")
	}
	select {
	case ch <- struct{}{}:
	default:
	}
}

// WatchEngine 排程桌面层序: 就绪 socket 常备 (Prepare 已提前拉起, 此处
// 兜底) → 引擎出现 → 等待就绪数据报 → reorder 一次 → 进入 DONE, 直到
// 引擎消失重置代际。引擎中途死亡则放弃本代并回到引擎等待。存在性轮询
// 1s 粒度仅用于代际边界判定。
// engineNames 为引擎二进制的精确基名 (如 "linux-wallpaperengine";
// 不可作前缀/子串匹配, 否则会命中 linux-wallpaperengine-gui)。
func WatchEngine(engineNames []string, logf func(string, ...any)) {
	startReadySocket()
	prev := map[int]bool{}
	for {
		cur := enginePIDs(engineNames)
		// 代际判定用集合差异而非有无边沿: GUI 切换壁纸时杀旧+起新可能
		// 发生在单次轮询间隙内, 有无边沿会漏检; pid 集合变化则必然可见。
		generationChanged := len(cur) > 0 && !maps.Equal(cur, prev)
		if generationChanged {
			tokenMu.Lock()
			tokenWaitChan = make(chan struct{}, 1)
			tokenMu.Unlock()

			deadline := time.After(60 * time.Second)
			waiting := true
			for waiting {
				select {
				case <-tokenWaitChan:
					waiting = false
					if err := Reorder(); err != nil && logf != nil {
						logf("[background] reorder: %v", err)
					} else if logf != nil {
						logf("[background] desktop reordered above engine (surface ready)")
					}
				case <-deadline:
					waiting = false
					if logf != nil {
						logf("[background] engine did not report surface readiness in 60s, generation abandoned")
					}
				default:
					cur = enginePIDs(engineNames)
					if len(cur) == 0 {
						waiting = false // 引擎先死了, 代际作废
					} else {
						time.Sleep(200 * time.Millisecond)
					}
				}
			}
			// 代际收尾必须置空: 残留的非 nil 通道会让 deliverToken 继续对
			// 每个数据报做投递与调试输出, 与已作废的代际错配
			tokenMu.Lock()
			tokenWaitChan = nil
			tokenMu.Unlock()
			cur = enginePIDs(engineNames)
		}
		prev = cur
		time.Sleep(time.Second)
	}
}
