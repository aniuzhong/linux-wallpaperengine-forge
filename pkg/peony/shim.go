package peony

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// gsettingsWallpaper 读取当前桌面壁纸路径 (去引号); 空串 = 无图模式。
// 契约: 指针只读, 绝不改写。
func gsettingsWallpaper() (string, error) {
	out, err := exec.Command("gsettings", "get", "org.mate.background", "picture-filename").Output()
	if err != nil {
		return "", fmt.Errorf("gsettings get picture-filename: %w", err)
	}
	return strings.Trim(strings.TrimSpace(string(out)), "'\""), nil
}

// wallpaperList 产出 shim 的匹配列表, 以及 Attach 是否应等待 nullified
// 日志行。无图用户天然兼容注入 (peony 届时不画任何不透明背景), 此时用
// accounts-service 哨兵值武装 shim —— 其目录级全放行规则兜住真实加载
// 路径。
func wallpaperList() (string, bool, error) {
	wallpaper, err := gsettingsWallpaper()
	if err != nil {
		return "", false, err
	}
	if wallpaper == "" {
		return accountsBgDir, false, nil
	}
	return wallpaper, true, nil
}

// locateShim 定位 libpeony-alpha.so: 环境变量 LWE_PEONY_SHIM 优先; 否则
// 从可执行文件目录逐级向上找 <dir>/lib/<名> 与 <dir>/<名> —— 同时覆盖
// 套件布局 (gui/resources → 套件根 lib/) 与扁平布局 (exe 同目录)。
func locateShim() (string, error) {
	if p := os.Getenv(shimPathEnv); p != "" {
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
		return "", fmt.Errorf("%s=%s does not exist", shimPathEnv, p)
	}
	exe, err := os.Executable()
	if err != nil {
		return "", fmt.Errorf("resolve executable: %w", err)
	}
	dir := filepath.Dir(exe)
	var tried []string
	for i := 0; i < 6 && dir != "/"; i++ {
		for _, candidate := range []string{
			filepath.Join(dir, "lib", shimMarker),
			filepath.Join(dir, shimMarker),
		} {
			if _, err := os.Stat(candidate); err == nil {
				return candidate, nil
			}
			tried = append(tried, candidate)
		}
		dir = filepath.Dir(dir)
	}
	return "", errors.New("libpeony-alpha.so not found (looked in:\n  " + strings.Join(tried, "\n  ") +
		"\nset " + shimPathEnv + " to override)")
}

// stageShim 把 shim 库重拷进状态目录 (与 shim 日志同目录, 路径保证无空格),
// 返回可直接放进 LD_PRELOAD 的路径。ld.so 按空白切分 LD_PRELOAD, 套件装在
// 含空格的路径下 (如 "Wallpaper Engine/") 时原路径必被拆散, 注入静默失败。
// 每轮 Attach 重拷, 套件更新自动跟随; 先写临时名再原子改名, 不给加载器留
// 半成品。源路径本身无空格时这是一次纯冗余的小文件拷贝, 可忽略。
func stageShim(src string) (string, error) {
	dst := filepath.Join(filepath.Dir(shimLogPath()), filepath.Base(src))
	if src == dst {
		return src, nil
	}
	data, err := os.ReadFile(src)
	if err != nil {
		return "", err
	}
	tmp := dst + ".staging"
	if err := os.WriteFile(tmp, data, 0o755); err != nil {
		return "", err
	}
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return "", err
	}
	return dst, nil
}
