// peonyctl —— pkg/peony 的开发验证工具。
//
// 仅供构建验证与手工排障使用,不随套件发布 (打包清单不收取)。
// 生产入口是 GUI 后端的 Attach/Detach;本工具直接驱动同一套 API,
// 真机验收因此不需要 GUI 在场。
//
// 用法:
//
//	peonyctl status   桌面壳 pid / 注入状态 / shim 定位
//	peonyctl attach   注入(幂等)
//	peonyctl detach   还原(幂等,只动被注入的实例)
//	peonyctl follow   跟踪 [inject] 日志流(Ctrl-C 退出)
package main

import (
	"fmt"
	"os"
	"os/signal"

	"lwe-forge/pkg/peony"
)

func main() {
	// 一律把日志桥接到 stdout:attach/detach 的进度与 follow 的数据同一条路
	peony.SetBridge(func(line string) { fmt.Println(line) })

	if len(os.Args) != 2 {
		usage()
	}
	switch os.Args[1] {
	case "status":
		status := peony.Inspect()
		fmt.Printf("peony    : pid %d", status.PID)
		if status.PID == 0 {
			fmt.Print(" (not running)")
		}
		fmt.Println()
		fmt.Printf("injected : %v\n", status.Injected)
		fmt.Printf("shim     : %s\n", status.ShimPath)
	case "attach":
		must(peony.Attach())
	case "detach":
		must(peony.Detach())
	case "follow":
		must(peony.EnableRelay(true))
		fmt.Println("[inject] following shim log (Ctrl-C to exit)")
		waitInterrupt()
		peony.DisableRelay()
	default:
		usage()
	}
}

func must(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func waitInterrupt() {
	c := make(chan os.Signal, 1)
	signal.Notify(c, os.Interrupt)
	<-c
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: peonyctl <status|attach|detach|follow>")
	os.Exit(2)
}
