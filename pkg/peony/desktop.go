package peony

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const (
	peonyDesktopMarker = "peony-qt-desktop"
	peonyDesktopArgv   = "/usr/bin/peony-qt-desktop -w -d"
	shimMarker         = "libpeony-alpha.so"
	// shimWallpaperEnv 是 shim 的惰性开关: 列表非空才武装。
	// accountsBgDir 前缀是 shim 内部的全放行规则。
	shimWallpaperEnv  = "PEONY_ALPHA_WALLPAPER"
	accountsBgDir     = "/var/lib/AccountsService/backgrounds/"
	singleInstanceGlb = "/tmp/qtsingleapp-peonyq*"
)

// IsPeonyDesktopCmdline 报告一段 /proc/<pid>/cmdline (NUL 分隔) 是否为
// 桌面壳。只比较 argv[0] 的 basename: 包装脚本、日志参数、恰好含该名字
// 的壁纸路径都不该命中 —— 本函数替换的子串判定曾误杀过携带该字面量的
// 包装 shell。
func IsPeonyDesktopCmdline(cmdline string) bool {
	fields := strings.Fields(strings.ReplaceAll(cmdline, "\x00", " "))
	if len(fields) == 0 {
		return false
	}
	return filepath.Base(fields[0]) == peonyDesktopMarker
}

// findPeonyPids 返回本 uid 的桌面壳进程 —— 另一会话的桌面壳轮不到我们碰。
func findPeonyPids() []int {
	var pids []int
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	selfUID := os.Getuid()
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		pid, err := strconv.Atoi(entry.Name())
		if err != nil {
			continue
		}
		if st, err := os.Stat("/proc/" + entry.Name()); err != nil || st.Sys().(*syscall.Stat_t).Uid != uint32(selfUID) {
			continue
		}
		cmdline, err := os.ReadFile("/proc/" + entry.Name() + "/cmdline")
		if err != nil {
			continue
		}
		if IsPeonyDesktopCmdline(string(cmdline)) {
			pids = append(pids, pid)
		}
	}
	return pids
}

// shimMapped 检查注入标志: libpeony-alpha.so 是否出现在
// /proc/<pid>/maps。os.ReadFile 读到 EOF 为止, 天然兼容 /proc 这类
// stat 尺寸为 0 的文件。
func shimMapped(pid int) bool {
	data, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/maps")
	return err == nil && strings.Contains(string(data), shimMarker)
}

// clearSingleInstanceLocks 清掉 peony 的单实例锁: 残留锁会让新实例以为
// 该把位置让给一个已不存在的桌面壳, 直接退出。
func clearSingleInstanceLocks() {
	matches, _ := filepath.Glob(singleInstanceGlb)
	for _, m := range matches {
		os.Remove(m)
	}
}

// stopPeony 终止本 uid 的全部桌面壳: 先 SIGTERM, 宽限期后对幸存者
// SIGKILL 收尾。
func stopPeony() {
	for _, pid := range findPeonyPids() {
		syscall.Kill(pid, syscall.SIGTERM)
	}
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if len(findPeonyPids()) == 0 {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	for _, pid := range findPeonyPids() {
		syscall.Kill(pid, syscall.SIGKILL)
	}
	time.Sleep(300 * time.Millisecond)
}

// baseEnv 返回剥离注入变量的基础环境 —— 干净重启的桌面壳必须不带任何
// 注入痕迹。桌面壳自身的注入变量只走 exec.Cmd.Env (每个子进程独立环境);
// 本包禁止 os.Setenv: 后端环境一旦被污染, 引擎等其他子进程会被误伤。
func baseEnv() []string {
	var out []string
	for _, kv := range os.Environ() {
		name, _, _ := strings.Cut(kv, "=")
		switch name {
		case "LD_PRELOAD", shimWallpaperEnv:
			continue
		}
		out = append(out, kv)
	}
	return out
}

// spawnPeony 以会话首进程身份分离拉起桌面壳, env 为该子进程的完整环境。
// Start 后交由 goroutine Wait 收尸, 避免退成僵尸。
func spawnPeony(env []string) error {
	argv := strings.Fields(peonyDesktopArgv)
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Env = env
	cmd.Stdin = nil
	cmd.Stdout = nil
	cmd.Stderr = nil
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("spawn %s: %w", peonyDesktopArgv, err)
	}
	go func() { _ = cmd.Wait() }()
	return nil
}
