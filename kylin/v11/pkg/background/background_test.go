package background

import (
	"net"
	"os"
	"testing"
	"time"
)

// armed 置一个待决代际 (与 WatchEngine 进入等待时一致), 返回还原函数。
func armed(t *testing.T) {
	t.Helper()
	tokenMu.Lock()
	tokenWaitChan = make(chan struct{}, 1)
	tokenMu.Unlock()
	t.Cleanup(func() {
		tokenMu.Lock()
		tokenWaitChan = nil
		tokenMu.Unlock()
	})
}

// tokenArmed 非阻塞读取令牌: true = 有, false = 无。
func tokenArmed() bool {
	tokenMu.Lock()
	ch := tokenWaitChan
	tokenMu.Unlock()
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

// startReadySocket 经 sync.Once 幂等; 测试里显式触发并要求环境注入成功。
func ensureReadySocket(t *testing.T) {
	t.Helper()
	startReadySocket()
	addr := os.Getenv("LWE_READY_SOCKET")
	if addr == "" {
		t.Fatal("LWE_READY_SOCKET not set: readiness socket did not start")
	}
}

func dialReady(t *testing.T) net.Conn {
	t.Helper()
	ensureReadySocket(t)
	conn, err := net.Dial("unixgram", os.Getenv("LWE_READY_SOCKET"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

// 核心回归: WatchEngine 的接收侧是带 default 的轮询 select, 永不泊车;
// 令牌通道必须带缓冲, 否则非阻塞发送永远失败, 数据报必丢, reorder 必不
// 触发 (桌面图标被引擎壁纸盖住的直接原因)。
func TestReadyDatagramDeliversTokenWithoutParkedReceiver(t *testing.T) {
	armed(t)
	conn := dialReady(t)
	if _, err := conn.Write([]byte(readyPayload)); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if tokenArmed() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("ready datagram was dropped: receiver was not parked on the channel")
}

// 非 READY 载荷不得产生令牌。
func TestReadySocketIgnoresForeignPayload(t *testing.T) {
	armed(t)
	conn := dialReady(t)
	if _, err := conn.Write([]byte("hello\n")); err != nil {
		t.Fatal(err)
	}
	time.Sleep(100 * time.Millisecond)
	if tokenArmed() {
		t.Fatal("foreign payload produced a token")
	}
}

// 无待决代际 (tokenWaitChan == nil) 时数据报被静默丢弃。
func TestDeliverTokenDropsWhenIdle(t *testing.T) {
	tokenMu.Lock()
	tokenWaitChan = nil
	tokenMu.Unlock()
	deliverToken() // 不得 panic
}

// 令牌容量 1: 同代际多个数据报只留一枚, reorder 只做一次。
func TestDeliverTokenCapacityOne(t *testing.T) {
	armed(t)
	deliverToken()
	deliverToken()
	if !tokenArmed() {
		t.Fatal("token lost")
	}
	if tokenArmed() {
		t.Fatal("duplicate datagram queued a second token")
	}
}
