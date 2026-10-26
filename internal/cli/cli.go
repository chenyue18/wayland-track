package cli

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"wayland-track/internal/daemon"
	"wayland-track/internal/takeover"
)

const defaultProtocolDirs = "/usr/share/wayland,/usr/share/wayland-protocols"

type commonOpts struct {
	reportDir     string
	protocolsDirs string
	noWayland     bool
	noX11         bool
	format        string
	maxMsgs       int
}

func addCommon(fs *flag.FlagSet, o *commonOpts) {
	fs.StringVar(&o.reportDir, "report-dir", "", "报告输出目录 (默认 $XDG_RUNTIME_DIR/wayland-track/reports)")
	fs.StringVar(&o.protocolsDirs, "protocols-dir", defaultProtocolDirs, "额外的协议 XML 目录，逗号分隔")
	fs.BoolVar(&o.noWayland, "no-wayland", false, "不监控 Wayland")
	fs.BoolVar(&o.noX11, "no-x11", false, "不监控 X11")
	fs.StringVar(&o.format, "format", "both", "报告格式: md | json | both")
	fs.IntVar(&o.maxMsgs, "max-msgs", 50000, "每条连接最多记录的消息数（统计不受影响）")
}

func runtimeDir() (string, error) {
	rd := os.Getenv("XDG_RUNTIME_DIR")
	if rd == "" {
		return "", fmt.Errorf("XDG_RUNTIME_DIR 未设置（需要在图形会话内运行）")
	}
	return rd, nil
}

func defaultReportDir(rd string) string {
	return filepath.Join(rd, "wayland-track", "reports")
}

func buildOptions(rd string, o *commonOpts) daemon.Options {
	repDir := o.reportDir
	if repDir == "" {
		repDir = defaultReportDir(rd)
	}
	var dirs []string
	if o.protocolsDirs != "" {
		for _, s := range strings.Split(o.protocolsDirs, ",") {
			s = strings.TrimSpace(s)
			if s != "" {
				dirs = append(dirs, s)
			}
		}
	}
	return daemon.Options{
		RuntimeDir:    rd,
		ReportDir:     repDir,
		ProtocolsDirs: dirs,
		NoWayland:     o.noWayland,
		NoX11:         o.noX11,
		Format:        o.format,
		MaxMsgs:       o.maxMsgs,
	}
}

func usage() {
	fmt.Print(`wayland-track — 记录应用使用的 Wayland / X11 协议

用法:
  wayland-track daemon [flags]     启动后台守护进程，接管 Wayland/X11 socket 并记录所有应用
  wayland-track launch [flags] -- CMD [ARGS...]
                                   以受控方式启动单个应用（通过环境变量把该应用导向代理 socket）
  wayland-track restore            恢复被守护进程接管的 socket（守护进程异常退出后使用）
  wayland-track status             查看守护进程与接管状态

daemon flags:
`)
}

func Main(args []string) int {
	if len(args) == 0 {
		usage()
		return 2
	}
	cmd := args[0]
	rest := args[1:]
	switch cmd {
	case "daemon":
		return cmdDaemon(rest)
	case "launch":
		return cmdLaunch(rest)
	case "restore":
		return cmdRestore(rest)
	case "status":
		return cmdStatus(rest)
	case "help", "-h", "--help":
		usage()
		return 0
	default:
		fmt.Fprintf(os.Stderr, "未知命令: %s\n\n", cmd)
		usage()
		return 2
	}
}

func cmdDaemon(args []string) int {
	fs := flag.NewFlagSet("daemon", flag.ExitOnError)
	var o commonOpts
	addCommon(fs, &o)
	displays := fs.String("displays", "", "只接管指定 socket，逗号分隔（如 wayland-0,X0）")
	daemonize := fs.Bool("d", false, "后台运行 (daemonize)")
	logFile := fs.String("log-file", "", "日志文件 (默认 $XDG_RUNTIME_DIR/wayland-track/daemon.log)")
	internal := fs.Bool("internal-daemon", false, "(内部使用)")
	_ = internal
	fs.Parse(args)

	rd, err := runtimeDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		return 1
	}

	if *daemonize && os.Getenv("WT_DAEMON_CHILD") == "" {
		logPath := *logFile
		if logPath == "" {
			logPath = filepath.Join(rd, "wayland-track", "daemon.log")
		}
		_ = os.MkdirAll(filepath.Dir(logPath), 0o755)
		lf, _ := os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		full := append(os.Args[1:], "-log-file", logPath)
		cmd := exec.Command(os.Args[0], full...)
		cmd.Env = append(os.Environ(), "WT_DAEMON_CHILD=1")
		cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
		cmd.Stdout = lf
		cmd.Stderr = lf
		cmd.Stdin = nil
		if err := cmd.Start(); err != nil {
			fmt.Fprintln(os.Stderr, "后台启动失败:", err)
			return 1
		}
		fmt.Printf("已在后台启动 (pid=%d)\n  日志: %s\n  报告: %s\n", cmd.Process.Pid, logPath, reportDirOr(o.reportDir, rd))
		return 0
	}

	opts := buildOptions(rd, &o)
	opts.Displays = nil
	if *displays != "" {
		opts.Displays = strings.Split(*displays, ",")
	}
	if os.Getenv("WT_DAEMON_CHILD") != "" {
		lf, err := os.OpenFile(*logFile, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		if err == nil {
			opts.LogOutput = lf
		} else {
			opts.LogOutput = os.Stderr
		}
	} else if *logFile != "" {
		_ = os.MkdirAll(filepath.Dir(*logFile), 0o755)
		lf, err := os.OpenFile(*logFile, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		if err == nil {
			opts.LogOutput = lf
		}
	}
	if opts.LogOutput == nil {
		opts.LogOutput = os.Stderr
	}

	d, err := daemon.New(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		return 1
	}
	return runDaemon(d)
}

func reportDirOr(dir, rd string) string {
	if dir != "" {
		return dir
	}
	return defaultReportDir(rd)
}

func cmdLaunch(args []string) int {
	fs := flag.NewFlagSet("launch", flag.ExitOnError)
	var o commonOpts
	addCommon(fs, &o)
	fs.Parse(args)
	cmdArgs := fs.Args()
	if len(cmdArgs) > 0 && cmdArgs[0] == "--" {
		cmdArgs = cmdArgs[1:]
	}
	if len(cmdArgs) == 0 {
		fmt.Fprintln(os.Stderr, "用法: wayland-track launch [flags] -- CMD [ARGS...]")
		return 2
	}
	rd, err := runtimeDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		return 1
	}
	opts := daemon.LaunchOptions{
		Options: buildOptions(rd, &o),
		Command: cmdArgs,
	}
	opts.LogOutput = os.Stderr
	if err := daemon.RunLaunch(opts); err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		return 1
	}
	return 0
}

func cmdRestore(args []string) int {
	fs := flag.NewFlagSet("restore", flag.ExitOnError)
	fs.Parse(args)
	rd, err := runtimeDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		return 1
	}
	if pid := daemonPid(rd); pid > 0 {
		fmt.Printf("守护进程 pid=%d 正在运行，发送 SIGTERM...", pid)
		syscall.Kill(pid, syscall.SIGTERM)
		for i := 0; i < 30; i++ {
			if !alive(pid) {
				break
			}
			time.Sleep(100 * time.Millisecond)
		}
		fmt.Println(" 已停止")
	}
	msgs, err := takeover.Restore(rd)
	if err != nil {
		fmt.Println(err)
		return 0
	}
	for _, m := range msgs {
		fmt.Println(m)
	}
	return 0
}

func cmdStatus(args []string) int {
	fs := flag.NewFlagSet("status", flag.ExitOnError)
	fs.Parse(args)
	rd, err := runtimeDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		return 1
	}
	if pid := daemonPid(rd); pid > 0 && alive(pid) {
		fmt.Printf("守护进程: 运行中 (pid=%d)\n", pid)
	} else {
		fmt.Println("守护进程: 未运行")
	}
	st := takeover.LoadState(rd)
	if st == nil {
		fmt.Println("接管状态: 无")
		return 0
	}
	fmt.Printf("接管状态: %d 个 socket\n", len(st.Targets))
	for _, t := range st.Targets {
		fmt.Printf("  [%s] %s -> %s\n", t.Proto, t.Proxy, t.Real)
	}
	fmt.Printf("报告目录: %s\n", st.ReportDir)
	return 0
}

func daemonPid(rd string) int {
	b, err := os.ReadFile(filepath.Join(rd, "wayland-track", "daemon.pid"))
	if err != nil {
		return 0
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil {
		return 0
	}
	return pid
}

func alive(pid int) bool {
	p, err := os.FindProcess(pid)
	if err != nil {
		return false
	}
	return p.Signal(syscall.Signal(0)) == nil
}

func daemonPidAlive(rd string) int {
	b, err := os.ReadFile(filepath.Join(rd, "wayland-track", "daemon.pid"))
	if err != nil {
		return 0
	}
	pid, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	if pid <= 0 {
		return 0
	}
	if p, err := os.FindProcess(pid); err == nil && p.Signal(syscall.Signal(0)) == nil {
		return pid
	}
	return 0
}

func runDaemon(d *daemon.Daemon) int {
	if err := d.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "错误:", err)
		return 1
	}
	return 0
}
