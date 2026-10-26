package daemon

import (
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"wayland-track/internal/procinfo"
	"wayland-track/internal/protocol"
	"wayland-track/internal/relay"
	"wayland-track/internal/report"
	"wayland-track/internal/takeover"
	"wayland-track/internal/wayland"
	"wayland-track/internal/x11"
)

type Options struct {
	RuntimeDir    string
	ReportDir     string
	ProtocolsDirs []string
	NoWayland     bool
	NoX11         bool
	Format        string
	MaxMsgs       int
	LogOutput     io.Writer
	Displays      []string
}

type Daemon struct {
	opts    Options
	db      *protocol.DB
	tracker *report.Tracker
	logger  *log.Logger
	targets []takeover.Target
	lns     []net.Listener
	sigCh   chan os.Signal
	stop    chan struct{}
	done    chan struct{}
	once    sync.Once
}

func LoadDB(dirs []string) *protocol.DB {
	db := protocol.New()
	if err := db.LoadEmbedded(); err != nil {
		log.Printf("加载内嵌协议定义失败: %v", err)
	}
	n := db.LoadDirs(dirs)
	log.Printf("协议定义: %d 个接口来自 %d 个内嵌 XML，额外加载 %d 个系统 XML", len(db.IfaceNames()), len(db.Sources), n)
	return db
}

func New(opts Options) (*Daemon, error) {
	logger := log.New(opts.LogOutput, "wayland-track: ", log.LstdFlags|log.Lmicroseconds)
	if opts.MaxMsgs <= 0 {
		opts.MaxMsgs = 50000
	}
	if opts.Format == "" {
		opts.Format = "both"
	}
	db := LoadDB(opts.ProtocolsDirs)
	tracker, err := report.NewTracker(opts.ReportDir, opts.Format, opts.MaxMsgs, logger)
	if err != nil {
		return nil, err
	}
	return &Daemon{
		opts:    opts,
		db:      db,
		tracker: tracker,
		logger:  logger,
		sigCh:   make(chan os.Signal, 2),
		stop:    make(chan struct{}),
		done:    make(chan struct{}),
	}, nil
}

func (d *Daemon) Stop() {
	close(d.stop)
}

func (d *Daemon) FinalizeNow() {
	d.tracker.FinalizeAll()
}

func (d *Daemon) Run() error {
	if b, err := os.ReadFile(d.PidfilePath()); err == nil {
		if pid, _ := strconv.Atoi(strings.TrimSpace(string(b))); pid > 0 {
			if p, e := os.FindProcess(pid); e == nil && p.Signal(syscall.Signal(0)) == nil {
				return fmt.Errorf("已有守护进程在运行 (pid=%d)，请先执行 wayland-track restore 或等待其退出", pid)
			}
		}
	}
	if takeover.LoadState(d.opts.RuntimeDir) != nil {
		msgs, _ := takeover.Restore(d.opts.RuntimeDir)
		for _, m := range msgs {
			d.logger.Printf("清理上次残留: %s", m)
		}
	}
	targets := takeover.Discover(d.opts.RuntimeDir, !d.opts.NoWayland, !d.opts.NoX11)
	if d.opts.Displays != nil {
		filtered := targets[:0]
		for _, t := range targets {
			for _, want := range d.opts.Displays {
				if t.Display == want || t.Proxy == want {
					filtered = append(filtered, t)
					break
				}
			}
		}
		targets = filtered
	}
	if len(targets) == 0 {
		return fmt.Errorf("没有发现可接管的 Wayland/X11 socket（RuntimeDir=%s）", d.opts.RuntimeDir)
	}

	state := &takeover.State{Pid: os.Getpid(), ReportDir: d.opts.ReportDir}
	for _, t := range targets {
		ln, err := takeover.Takeover(t)
		if err != nil {
			d.logger.Printf("跳过 %s: %v", t.Proxy, err)
			continue
		}
		d.targets = append(d.targets, t)
		d.lns = append(d.lns, ln)
		state.Targets = d.targets
		if err := takeover.SaveState(d.opts.RuntimeDir, state); err != nil {
			d.logger.Printf("保存状态文件失败: %v", err)
		}
		d.logger.Printf("已接管 %s [%s]，真实 socket -> %s", t.Proxy, t.Proto, t.Real)
	}
	if len(d.lns) == 0 {
		return fmt.Errorf("所有 socket 接管均失败，未开始监控")
	}
	d.writePidfile()

	for i, ln := range d.lns {
		t := d.targets[i]
		go d.acceptLoop(ln, t)
	}
	for i := range d.targets {
		if d.targets[i].Proto != "X11" {
			continue
		}
		lnA, err := net.ListenUnix("unix", &net.UnixAddr{Name: "@" + d.targets[i].Proxy, Net: "unix"})
		if err == nil {
			d.logger.Printf("已额外接管 abstract socket @%s，X11 客户端将完整经过代理", d.targets[i].Proxy)
			d.lns = append(d.lns, lnA)
			d.targets = append(d.targets, d.targets[i])
			go d.acceptLoop(lnA, d.targets[len(d.targets)-1])
		} else if errors.Is(err, syscall.EADDRINUSE) {
			d.logger.Printf("警告: X server 同时监听 abstract socket @%s，X11 客户端可能绕过代理；如需可靠监控 X11 应用请使用 launch 模式",
				d.targets[i].Proxy)
		}
	}
	d.logger.Printf("守护进程运行中 (pid=%d)，报告目录: %s", os.Getpid(), d.opts.ReportDir)

	signal.Notify(d.sigCh, syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP, syscall.SIGQUIT)
	sweep := time.NewTicker(2 * time.Second)
	defer sweep.Stop()

	for {
		select {
		case <-d.sigCh:
			d.shutdown()
			return nil
		case <-d.stop:
			d.shutdown()
			return nil
		case <-sweep.C:
			d.tracker.Sweep()
		case <-d.done:
			return nil
		}
	}
}

func (d *Daemon) writePidfile() {
	p := filepath.Join(d.opts.RuntimeDir, "wayland-track", "daemon.pid")
	_ = os.MkdirAll(filepath.Dir(p), 0o755)
	_ = os.WriteFile(p, []byte(fmt.Sprintf("%d\n", os.Getpid())), 0o644)
}

func (d *Daemon) PidfilePath() string {
	return filepath.Join(d.opts.RuntimeDir, "wayland-track", "daemon.pid")
}

func (d *Daemon) shutdown() {
	d.once.Do(func() {
		close(d.done)
	})
	for _, ln := range d.lns {
		ln.Close()
	}
	d.tracker.FinalizeAll()
	msgs, err := takeover.Restore(d.opts.RuntimeDir)
	if err != nil {
		d.logger.Printf("恢复 socket: %v", err)
	}
	for _, m := range msgs {
		d.logger.Printf("%s", m)
	}
	os.Remove(d.PidfilePath())
	d.logger.Printf("守护进程已退出，socket 已恢复")
}

func (d *Daemon) newParser(proto string) report.Parser {
	if proto == "Wayland" {
		return wayland.New(d.db)
	}
	return x11.New()
}

func (d *Daemon) acceptLoop(ln net.Listener, t takeover.Target) {
	for {
		c, err := ln.(*net.UnixListener).AcceptUnix()
		if err != nil {
			return
		}
		go d.handleConn(c, t)
	}
}

func (d *Daemon) handleConn(c *net.UnixConn, t takeover.Target) {
	defer c.Close()
	pid, uid, gid, err := takeover.PeerCred(c)
	if err != nil {
		d.logger.Printf("获取对端凭据失败: %v", err)
		return
	}
	app := procinfo.Read(pid, uid, gid)

	realConn, err := dialReal(t.Real)
	if err != nil {
		d.logger.Printf("连接真实 socket %s 失败: %v", t.Real, err)
		return
	}
	defer realConn.Close()

	parser := d.newParser(t.Proto)
	connID := d.tracker.OpenConn(pid, t.Proto, t.Proxy, app, parser)

	var wg sync.WaitGroup
	wg.Add(2)
	bothClosed := func() { wg.Done() }
	go relay.Pump(c, realConn, relay.DirC2S, parser, d.tracker, connID, bothClosed)
	go relay.Pump(realConn, c, relay.DirS2C, parser, d.tracker, connID, bothClosed)
	wg.Wait()
	d.tracker.CloseConn(connID)
}

func dialReal(path string) (*net.UnixConn, error) {
	c, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: path, Net: "unix"})
	if err == nil {
		return c, nil
	}
	return net.DialUnix("unix", nil, &net.UnixAddr{Name: "@" + path, Net: "unix"})
}

func SplitDirs(list string) []string {
	var out []string
	for _, s := range strings.Split(list, ",") {
		s = strings.TrimSpace(s)
		if s != "" {
			out = append(out, s)
		}
	}
	return out
}
