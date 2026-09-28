// peony 包的状态机: Attach / Detach / Inspect, 以及日志桥。
package peony

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// Status 是桌面壳与注入状态的快照。
type Status struct {
	PID      int    // 0 = 桌面壳未运行
	Injected bool   // shim 是否已映射进桌面壳
	ShimPath string // 定位到的 shim 库; 空 = 未找到
}

var (
	mu sync.Mutex // Attach/Detach/Inspect 的进程内串行化
	// bridgeMu 与状态机锁刻意分离: logf 会在 Attach/Detach 持 mu 的过程中
	// 被调用, 共用一把锁就是自锁死锁。
	bridgeMu sync.RWMutex
	bridge   func(string)
)

// SetBridge 注入日志桥 (宿主 GUI 传 logger.Println 一类函数)。行已带
// "[inject] " 前缀, 桥只管转投; nil 桥时本包静默。须在 Attach 前调用。
func SetBridge(fn func(string)) {
	bridgeMu.Lock()
	defer bridgeMu.Unlock()
	bridge = fn
}

// logf 是本包的唯一输出通道: 拼好前缀交桥; 桥出错与宿主无关, 吞掉。
func logf(format string, args ...any) {
	bridgeMu.RLock()
	fn := bridge
	bridgeMu.RUnlock()
	if fn == nil {
		return
	}
	fn("[inject] " + fmt.Sprintf(format, args...))
}

// Inspect 返回当前状态快照; shim 定位失败不视为错误 (ShimPath 留空)。
func Inspect() Status {
	mu.Lock()
	defer mu.Unlock()
	status := Status{}
	if pids := findPeonyPids(); len(pids) > 0 {
		status.PID = pids[0]
		status.Injected = shimMapped(status.PID)
	}
	if p, err := locateShim(); err == nil {
		status.ShimPath = p
	}
	return status
}

// Attach 使桌面壳透明 (幂等):
//
//  1. 已注入 → 确保 relay 在跑 (replay 上代日志), 原样返回;
//  2. 定位 shim、读取当前壁纸路径 (只读, 不改指针);
//  3. 终止桌面壳 (TERM→3s→KILL)、清单实例锁;
//  4. 携带注入环境分离拉起 (日志文件同轮清旧);
//  5. 轮询验证 ≤12s: maps 命中, 且 (有壁纸时) 日志出现 nullified 行。
func Attach() error {
	mu.Lock()
	defer mu.Unlock()

	if pids := findPeonyPids(); len(pids) > 0 && shimMapped(pids[0]) {
		logf("attach: peony pid %d already injected, nothing to do", pids[0])
		EnableRelay(true)
		return nil
	}

	shimPath, err := locateShim()
	if err != nil {
		logf("attach failed: %v", err)
		return err
	}
	list, expectNullify, err := wallpaperList()
	if err != nil {
		logf("attach failed: %v", err)
		return err
	}
	if !expectNullify {
		logf("attach: no picture wallpaper set, arming shim with accounts-dir sentinel")
	}

	// 每轮注入一份数代日志: 清旧, 失败验证才不会读到上一代的证据链。
	// shim 只 append 不建目录, 日志目录由本模块负责创建 —— 缺了它 shim
	// 会静默关闭日志, 新机器上注入验证将误报失败。
	logPath := shimLogPath()
	if err := os.MkdirAll(filepath.Dir(logPath), 0o755); err != nil {
		logf("attach: cannot create log directory %s: %v", filepath.Dir(logPath), err)
	}
	if err := os.Remove(logPath); err != nil && !errors.Is(err, os.ErrNotExist) {
		logf("attach: cannot reset shim log %s: %v", logPath, err)
	}

	logf("attach: stopping peony")
	stopPeony()
	clearSingleInstanceLocks()

	env := append(baseEnv(),
		"LD_PRELOAD="+shimPath,
		shimWallpaperEnv+"="+list,
	)
	if err := spawnPeony(env); err != nil {
		logf("attach failed: %v", err)
		return err
	}

	pid, err := waitInjected(logPath, expectNullify, 12*time.Second)
	if err != nil {
		logf("attach failed: %v", err)
		return err
	}
	logf("attach: peony pid %d injected via %s", pid, shimPath)
	EnableRelay(false)
	return nil
}

// waitInjected 轮询确认: 进程在、shim 在 maps 里、且 (有壁纸时) 日志出现
// nullified 行 —— maps 只证明库加载, nullified 才证明钩子真命中了壁纸。
func waitInjected(logPath string, expectNullify bool, timeout time.Duration) (int, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		time.Sleep(200 * time.Millisecond)
		pids := findPeonyPids()
		if len(pids) == 0 {
			continue
		}
		if !shimMapped(pids[0]) {
			continue
		}
		if expectNullify {
			data, err := os.ReadFile(logPath)
			if err != nil || !strings.Contains(string(data), "nullified wallpaper pixmap") {
				continue
			}
		}
		return pids[0], nil
	}
	return 0, fmt.Errorf("peony did not come up injected within %s (log: %s)", timeout, logPath)
}

// Detach 还原桌面壳 (幂等, 只在确有注入时动手): 终止被注入的实例、清锁、
// 以剥离注入变量的环境分离拉起干净实例, 并确认新实例 maps 无 shim。
// 已在干净运行的桌面壳原样保留 —— 重启只会无谓刷新用户的桌面图标。
func Detach() error {
	mu.Lock()
	defer mu.Unlock()

	pids := findPeonyPids()
	if len(pids) == 0 {
		logf("detach: peony not running, nothing to do")
		return nil
	}
	if !shimMapped(pids[0]) {
		logf("detach: peony pid %d is clean, leaving it alone", pids[0])
		return nil
	}

	DisableRelay()
	logf("detach: stopping injected peony pid %d", pids[0])
	stopPeony()
	clearSingleInstanceLocks()

	if err := spawnPeony(baseEnv()); err != nil {
		logf("detach failed: %v", err)
		return err
	}

	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		time.Sleep(200 * time.Millisecond)
		fresh := findPeonyPids()
		if len(fresh) == 0 {
			continue
		}
		if shimMapped(fresh[0]) {
			continue
		}
		logf("detach: peony pid %d restored clean", fresh[0])
		return nil
	}
	err := errors.New("peony relaunched but shim is still mapped (or it did not come up)")
	logf("detach failed: %v", err)
	return err
}
