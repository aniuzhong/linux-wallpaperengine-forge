package background

import (
	"fmt"
	"strings"
	"testing"
)

// armed 置一个待决代际 (与 WatchEngine 进入等待时一致), 返回还原函数。
func armed(t *testing.T) {
	t.Helper()
	markerMu.Lock()
	markerWaitChan = make(chan struct{}, 1)
	markerMu.Unlock()
	t.Cleanup(func() {
		markerMu.Lock()
		markerWaitChan = nil
		markerMu.Unlock()
	})
}

// tokenArmed 非阻塞读取令牌: true = 有, false = 无。
func tokenArmed() bool {
	markerMu.Lock()
	ch := markerWaitChan
	markerMu.Unlock()
	if ch == nil {
		return false
	}
	select {
	case <-ch:
		return true
	default:
		return false
	}
}

// 核心回归: WatchEngine 的接收侧是带 default 的轮询 select, 永不泊车;
// 令牌通道必须带缓冲, 否则非阻塞发送永远失败, marker 必丢, reorder 必不
// 触发 (桌面图标被引擎壁纸盖住的直接原因)。
func TestIngestLineDeliversMarkerWithoutParkedReceiver(t *testing.T) {
	armed(t)
	IngestLine("Virtual-1: " + mappedMarker)
	if !tokenArmed() {
		t.Fatal("marker token was dropped: receiver was not parked on the channel")
	}
}

// 非 marker 行 (含启动历史回放中的任意行) 不得产生令牌。
func TestIngestLineIgnoresNonMarkerLines(t *testing.T) {
	armed(t)
	for _, line := range []string{
		"Starting wallpaper for Virtual-1...",
		"[ELECTRON] Starting Go backend from Electron",
		"[lwe] wayland output map", // 前缀相似但不是完整 marker
		"",
	} {
		IngestLine(line)
		if tokenArmed() {
			t.Fatalf("non-marker line produced a token: %q", line)
		}
	}
}

// 无待决代际 (markerWaitChan == nil) 时一切输入都被忽略 —— Subscribe 的
// 历史回放因此天然无害。
func TestIngestLineIgnoresWhenIdle(t *testing.T) {
	markerMu.Lock()
	markerWaitChan = nil
	markerMu.Unlock()
	IngestLine("Virtual-1: " + mappedMarker) // 不得 panic
}

// 调试出口只允许 marker 命中各打一行, 且文本不得包含 marker 本身 ——
// 否则 debugLogf(→logger) 再喂回 IngestLine 就是自激递归, 会瞬间灌满
// 日志历史并烧满 CPU (实测后端 90%+)。
func TestIngestLineDebugLogFiresOnlyOnMarker(t *testing.T) {
	armed(t)
	var debugged []string
	SetDebugLogf(func(format string, args ...any) {
		debugged = append(debugged, fmt.Sprintf(format, args...))
	})
	t.Cleanup(func() { SetDebugLogf(nil) })

	IngestLine("some ordinary engine line")
	if len(debugged) != 0 {
		t.Fatalf("debug log fired for a non-marker line: %v", debugged)
	}
	IngestLine("Virtual-1: " + mappedMarker)
	if len(debugged) != 1 {
		t.Fatalf("debug log fired %d times for one marker, want 1: %v", len(debugged), debugged)
	}
	if strings.Contains(debugged[0], mappedMarker) {
		t.Fatalf("debug text echoes the marker itself, would re-trigger: %q", debugged[0])
	}
}

// marker 到达后, 第二个 marker 不得再塞令牌 (容量 1, 防代际内重复 reorder)。
func TestIngestLineTokenCapacityOne(t *testing.T) {
	armed(t)
	IngestLine("Virtual-1: " + mappedMarker)
	IngestLine("Virtual-1: " + mappedMarker)
	if !tokenArmed() {
		t.Fatal("token lost after duplicate marker")
	}
	// 取走令牌后不应还有余量
	if tokenArmed() {
		t.Fatal("duplicate marker queued a second token")
	}
}
