// Package background —— 麒麟 V11 (kylin-wlcom + peony 4.21) 的壁纸契约。
//
// V11 桌面形态: peony-qt-desktop 以 wlr-layer-shell background 层绘制壁纸与
// 图标; 引擎同样挂 background 层, 同层内后映射者居上。方案:
//
//	Prepare : 备份用户壁纸三元组 → gsettings 指向全透明 PNG → peony 重绘
//	Reorder : peony-qt-desktop -u —— 令 peony 表面重建, 图标层抬回引擎上方
//	Detach  : 还原用户壁纸
//
// 零注入: 与 peony 的全部交互是 gsettings 键与 -u 命令行, 均为公开契约。
// 引擎时序由 WatchEngine 驱动 (进程出现 → 延时 → Reorder), 对 GUI 上游
// 代码零侵入。
package background

import (
	"encoding/json"
	"errors"
	"maps"
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
func Prepare() (string, error) {
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

// engineRunning 扫描 /proc, 按 exe 符号链接的基名精确匹配引擎二进制。
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

func engineRunning(names []string) bool {
	return len(enginePIDs(names)) > 0
}

// ---- 确定性时序: 表面映射标记驱动 ---------------------------------------------
//
// 引擎 (0002-wayland-map-log 补丁) 在 layer surface ack_configure 时向
// stderr 打一行 mappedMarker, GUI 捕获后经 logger 广播; IngestLine 把它
// 变成一次性的 reorder 触发。整条链路没有经验睡眠值: reorder 精确发生在
// "引擎表面已进入合成器场景"这一事件之后。
const mappedMarker = "[lwe] wayland output mapped"

var (
	markerMu       sync.Mutex
	markerWaitChan chan struct{} // 每个引擎代际一个 (容量 1); nil = 当前无待决 reorder
	debugLogf      func(string, ...any)
)

// SetDebugLogf 装配 marker 命中的单行诊断出口 (生产由 GUI 侧 002 补丁
// 接线传 logger.Printf)。仅命中时打点, 任意行不得放行 (见 IngestLine)。
func SetDebugLogf(f func(string, ...any)) { debugLogf = f }

// IngestLine 供宿主把 GUI logger 的广播行喂进来 (logger.Subscribe 的
// 转发协程)。只消费 mappedMarker, 且仅在等待标记的代际内生效 ——
// Subscribe 启动时的历史回放因此天然被忽略。
//
// 切勿对任意行 debugLog: 出口即 logger, logger 再喂回本函数会形成
// ingest → log → ingest 自激递归, 瞬间灌满 500 条历史环形缓冲并烧 CPU
// (实测后端 CPU 90%+, 日志页/日志 socket 全被嵌套垃圾淹没)。
func IngestLine(line string) {
	markerMu.Lock()
	ch := markerWaitChan
	markerMu.Unlock()
	if ch == nil || !strings.Contains(line, mappedMarker) {
		return
	}
	if debugLogf != nil {
		// 提示文本不得含 mappedMarker 本身, 否则这行也会再次触发
		debugLogf("[background] engine surface-mapped marker observed, scheduling desktop reorder")
	}
	// 容量 1 的令牌: WatchEngine 的接收侧是 200ms 轮询 select(带 default,
	// 永不泊车), 无缓冲通道的非阻塞发送永远失败 —— marker 必丢, reorder
	// 必不触发 (图标被引擎盖住的直接原因)。带缓冲后令牌在此等待下一拍。
	select {
	case ch <- struct{}{}:
	default:
	}
}

// WatchEngine 排程桌面层序: 引擎出现 → 等待映射标记 (由 IngestLine 喂入)
// → reorder 一次 → 进入 DONE, 直到引擎消失重置代际。引擎中途死亡则放弃
// 本代并回到引擎等待。存在性轮询 1s 粒度仅用于代际边界判定。
// engineNames 为引擎二进制的精确基名 (如 "linux-wallpaperengine";
// 不可作前缀/子串匹配, 否则会命中 linux-wallpaperengine-gui)。
func WatchEngine(engineNames []string, logf func(string, ...any)) {
	prev := map[int]bool{}
	for {
		cur := enginePIDs(engineNames)
		// 代际判定用集合差异而非有无边沿: GUI 切换壁纸时杀旧+起新可能
		// 发生在单次轮询间隙内, 有无边沿会漏检; pid 集合变化则必然可见。
		generationChanged := len(cur) > 0 && !maps.Equal(cur, prev)
		if generationChanged {
			markerMu.Lock()
			markerWaitChan = make(chan struct{}, 1) // 容量 1: 令牌在接收侧轮询间隙暂存
			markerMu.Unlock()

			deadline := time.After(60 * time.Second) // 映射超时的代际放弃线
			waiting := true
			for waiting {
				select {
				case <-markerWaitChan:
					waiting = false
					if err := Reorder(); err != nil && logf != nil {
						logf("[background] reorder: %v", err)
					} else if logf != nil {
						logf("[background] desktop reordered above engine (surface mapped)")
					}
				case <-deadline:
					waiting = false
					if logf != nil {
						logf("[background] engine did not report surface mapping in 60s, generation abandoned")
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
			// 代际收尾必须置空: 残留的非 nil 通道会让 IngestLine 继续对每行
			// 日志做 marker 匹配与调试输出, 若不拦还与 logger 构成自激回路
			markerMu.Lock()
			markerWaitChan = nil
			markerMu.Unlock()
			cur = enginePIDs(engineNames)
		}
		prev = cur
		time.Sleep(time.Second)
	}
}
