// tail.go —— shim 日志的纯转发中继。
//
// 职责边界 (刻意收窄): 本文件只做"把 shim 日志的新行搬到桥上"这一件事,
// 不驱动任何重注入决策 —— 它是渲染需要, 不是 watcher。注入丢失时这里的
// 表现是"代际沉默", 不是自作主张的修复。
//
// 轮询而非 inotify: 零依赖; 日志体量本就极小 (每代几行), 500ms 粒度足够。
package peony

import (
	"os"
	"strings"
	"sync"
	"time"
)

// shimLogPath 是 shim 源码 (src/peony-qt-desktop/peony-alpha.cpp) 写死的
// 目的地, 两侧必须一致 —— 改一处必改另一处。
func shimLogPath() string {
	dataHome := os.Getenv("XDG_DATA_HOME")
	if dataHome == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			return ""
		}
		dataHome = home + "/.local/share"
	}
	return dataHome + "/lwe-forge/peony-alpha.log"
}

// tailState 是中继的游标: offset 之上是已转发内容, partial 暂存半行。
type tailState struct {
	offset  int64
	partial string
}

// pollOnce 把文件自 offset 起的新内容按行搬给 emit; 半行留到下次。
// 文件变小 (被清旧/轮转) 即视为新一轮, 从头重放。路径不存在静默等待。
func pollOnce(path string, state *tailState, emit func(string)) error {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	if info.Size() < state.offset {
		state.offset = 0
		state.partial = ""
	}
	if info.Size() == state.offset {
		return nil
	}
	if _, err := f.Seek(state.offset, 0); err != nil {
		return err
	}
	buf := make([]byte, 8192)
	var total int64
	for {
		n, err := f.Read(buf)
		if n > 0 {
			chunk := state.partial + string(buf[:n])
			lines := strings.SplitAfter(chunk, "\n")
			state.partial = lines[len(lines)-1]
			for _, line := range lines[:len(lines)-1] {
				emit(strings.TrimRight(line, "\r\n"))
			}
			total += int64(n)
		}
		if err != nil || n == 0 {
			break
		}
	}
	state.offset += total
	return nil
}

var (
	relayOnce sync.Mutex
	relayStop chan struct{}
)

// EnableRelay 启动转发中继 (幂等)。replay=true 从文件头重放 (用于"发现
// 已注入"的场景, 把上一代的证据链搬进 UI); false 只搬新行。
func EnableRelay(replay bool) error {
	relayOnce.Lock()
	defer relayOnce.Unlock()
	if relayStop != nil {
		return nil
	}
	path := shimLogPath()
	if path == "" {
		return nil
	}
	state := &tailState{}
	if !replay {
		if info, err := os.Stat(path); err == nil {
			state.offset = info.Size()
		}
	}
	stop := make(chan struct{})
	relayStop = stop
	go func() {
		ticker := time.NewTicker(500 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-stop:
				return
			case <-ticker.C:
				relayOnce.Lock()
				stopCopy := relayStop
				relayOnce.Unlock()
				if stopCopy != stop {
					return
				}
				bridgeMu.RLock()
				fn := bridge
				bridgeMu.RUnlock()
				_ = pollOnce(path, state, func(line string) {
					if line != "" && fn != nil {
						fn("[inject] " + line)
					}
				})
			}
		}
	}()
	return nil
}

// DisableRelay 停止转发中继 (幂等)。
func DisableRelay() {
	relayOnce.Lock()
	defer relayOnce.Unlock()
	if relayStop != nil {
		close(relayStop)
		relayStop = nil
	}
}
