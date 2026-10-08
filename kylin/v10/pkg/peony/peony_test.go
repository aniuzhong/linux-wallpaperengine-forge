package peony

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestIsPeonyDesktopCmdline(t *testing.T) {
	cases := []struct {
		cmdline string
		want    bool
	}{
		{"/usr/bin/peony-qt-desktop -w -d", true},
		{"/usr/bin/peony-qt-desktop", true},
		{"peony-qt-desktop\x00-w\x00-d", true}, // cmdline 以 NUL 分隔
		{"/usr/bin/peony file:///tmp", false},  // 文件管理器窗口不是桌面壳
		{"", false},
		{"ukui-kwin --replace", false},
		// 携带该字面量的包装 shell (历史上真被误杀过): argv[0] 不是桌面壳
		{"/bin/bash -c pgrep -f peony-qt-desktop", false},
		{"/tmp/peonyctl attach", false},
	}
	for _, c := range cases {
		if got := IsPeonyDesktopCmdline(c.cmdline); got != c.want {
			t.Errorf("IsPeonyDesktopCmdline(%q) = %v, want %v", c.cmdline, got, c.want)
		}
	}
}

func TestBaseEnvStripsInjectionVars(t *testing.T) {
	t.Setenv("LD_PRELOAD", "/tmp/evil.so")
	t.Setenv(shimWallpaperEnv, "/tmp/x.jpg")
	t.Setenv("HOME", "/home/someone")

	env := baseEnv()
	joined := strings.Join(env, "\n")
	if strings.Contains(joined, "LD_PRELOAD=") {
		t.Errorf("baseEnv leaked LD_PRELOAD: %v", env)
	}
	if strings.Contains(joined, shimWallpaperEnv+"=") {
		t.Errorf("baseEnv leaked %s: %v", shimWallpaperEnv, env)
	}
	found := false
	for _, kv := range env {
		if kv == "HOME=/home/someone" {
			found = true
		}
	}
	if !found {
		t.Errorf("baseEnv dropped unrelated variable HOME: %v", env)
	}
}

func TestPollOnce(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "peony-alpha.log")

	// 文件不存在:静默等待,零行
	state := &tailState{}
	var lines []string
	emit := func(l string) { lines = append(lines, l) }
	if err := pollOnce(path, state, emit); err != nil {
		t.Fatalf("pollOnce on missing file: %v", err)
	}
	if len(lines) != 0 {
		t.Fatalf("expected no lines, got %v", lines)
	}

	// 整行写入:全部上交,offset 推进
	if err := os.WriteFile(path, []byte("line-a\nline-b\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := pollOnce(path, state, emit); err != nil {
		t.Fatal(err)
	}
	if len(lines) != 2 || lines[0] != "line-a" || lines[1] != "line-b" {
		t.Fatalf("got %v, want [line-a line-b]", lines)
	}

	// 半行:不上交,暂存;补全后拼行上交
	if err := os.WriteFile(path, []byte("line-a\nline-b\npartial"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := pollOnce(path, state, emit); err != nil {
		t.Fatal(err)
	}
	if len(lines) != 2 {
		t.Fatalf("partial line leaked early: %v", lines)
	}
	f, _ := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0o644)
	f.WriteString("ary\n")
	f.Close()
	if err := pollOnce(path, state, emit); err != nil {
		t.Fatal(err)
	}
	if lines[len(lines)-1] != "partialary" {
		t.Fatalf("partial line not reassembled: %v", lines)
	}

	// 文件被清旧重写(变小):视为新一轮,从头重放
	if err := os.WriteFile(path, []byte("fresh-1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := pollOnce(path, state, emit); err != nil {
		t.Fatal(err)
	}
	if lines[len(lines)-1] != "fresh-1" {
		t.Fatalf("truncation not detected, tail: %v", lines)
	}
}
