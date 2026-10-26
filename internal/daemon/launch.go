package daemon

import (
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	"wayland-track/internal/takeover"
)

type LaunchOptions struct {
	Options
	Command []string
}

func RunLaunch(opts LaunchOptions) error {
	if len(opts.Command) == 0 {
		return fmt.Errorf("launch 需要指定要启动的命令，例如: wayland-track launch -- gedit")
	}
	d, err := New(opts.Options)
	if err != nil {
		return err
	}
	d.logger.Printf("launch 模式: %v", opts.Command)

	childEnv := os.Environ()
	realWayland := ""
	realX := ""
	envOverrides := []string{}

	if !opts.NoWayland {
		wd := os.Getenv("WAYLAND_DISPLAY")
		if wd == "" {
			wd = "wayland-0"
		}
		realWayland = wd
		if !strings.HasPrefix(wd, "/") {
			realWayland = filepath.Join(opts.RuntimeDir, wd)
		}
		if !isSocket(realWayland) {
			d.logger.Printf("警告: Wayland socket 不存在: %s", realWayland)
			realWayland = ""
		} else {
			proxyDir := filepath.Join(opts.RuntimeDir, fmt.Sprintf("wayland-track-launch-%d", os.Getpid()))
			proxySock := filepath.Join(proxyDir, "wayland-0")
			_ = os.MkdirAll(proxyDir, 0o700)
			ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: proxySock, Net: "unix"})
			if err != nil {
				return fmt.Errorf("创建 Wayland 代理监听失败: %w", err)
			}
			d.lns = append(d.lns, ln)
			d.targets = append(d.targets, takeover.Target{Proto: "Wayland", Proxy: proxySock, Real: realWayland})
			envOverrides = append(envOverrides, "WAYLAND_DISPLAY="+proxyDir+"/wayland-0")
			go d.acceptLoop(ln, takeover.Target{Proto: "Wayland", Proxy: proxySock, Real: realWayland})
			d.logger.Printf("Wayland 代理: %s -> %s", proxySock, realWayland)
		}
	}

	if !opts.NoX11 {
		disp := os.Getenv("DISPLAY")
		if disp == "" {
			d.logger.Printf("警告: DISPLAY 未设置，跳过 X11 代理")
		} else {
			n := displayNumber(disp)
			if n < 0 {
				d.logger.Printf("警告: 无法解析 DISPLAY=%q，跳过 X11 代理", disp)
			} else {
				realX = fmt.Sprintf("/tmp/.X11-unix/X%d", n)
				proxyN := 90
				for ; proxyN < 100; proxyN++ {
					if !isSocket(fmt.Sprintf("/tmp/.X11-unix/X%d", proxyN)) {
						break
					}
				}
				if proxyN >= 100 {
					return fmt.Errorf("没有可用的 X11 代理显示号")
				}
				proxyPath := fmt.Sprintf("/tmp/.X11-unix/X%d", proxyN)
				ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: proxyPath, Net: "unix"})
				if err != nil {
					return fmt.Errorf("创建 X11 代理监听失败: %w", err)
				}
				_ = os.Chmod(proxyPath, 0o777)
				lnAbstract, err := net.ListenUnix("unix", &net.UnixAddr{Name: "@" + proxyPath, Net: "unix"})
				if err != nil {
					ln.Close()
					return fmt.Errorf("创建 X11 代理抽象监听失败: %w", err)
				}
				d.lns = append(d.lns, ln, lnAbstract)
				d.targets = append(d.targets, takeover.Target{Proto: "X11", Proxy: proxyPath, Real: realX})
				envOverrides = append(envOverrides, "DISPLAY=:"+strconv.Itoa(proxyN))
				go d.acceptLoop(ln, takeover.Target{Proto: "X11", Proxy: proxyPath, Real: realX})
				go d.acceptLoop(lnAbstract, takeover.Target{Proto: "X11", Proxy: proxyPath, Real: realX})
				d.logger.Printf("X11 代理: :%d(fs+abstract) -> %s", proxyN, realX)
			}
		}
	}

	childEnv = append(childEnv, envOverrides...)
	cmd := exec.Command(opts.Command[0], opts.Command[1:]...)
	cmd.Env = childEnv
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}

	err = cmd.Start()
	if err != nil {
		closeListeners(d.lns)
		return fmt.Errorf("启动应用失败: %w", err)
	}
	childPid := cmd.Process.Pid
	d.logger.Printf("应用已启动 pid=%d，报告将在应用退出后写入 %s", childPid, opts.ReportDir)

	err = cmd.Wait()

	for _, t := range d.targets {
		if strings.Contains(t.Proxy, "wayland-track-launch") {
			os.Remove(t.Proxy)
			os.Remove(filepath.Dir(t.Proxy))
		} else if strings.HasPrefix(t.Proxy, "/tmp/.X11-unix/X9") {
			os.Remove(t.Proxy)
		}
	}
	closeListeners(d.lns)
	d.tracker.Sweep()
	d.tracker.FinalizeAll()
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			d.logger.Printf("应用退出: %s", ee.String())
		}
	} else {
		d.logger.Printf("应用正常退出")
	}
	d.logger.Printf("报告已写入 %s", opts.ReportDir)
	return nil
}

func closeListeners(lns []net.Listener) {
	for _, ln := range lns {
		ln.Close()
	}
}

func isSocket(path string) bool {
	fi, err := os.Stat(path)
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeSocket != 0
}

func displayNumber(disp string) int {
	s := disp
	if i := strings.Index(s, ":"); i >= 0 {
		s = s[i+1:]
	} else {
		return -1
	}
	if i := strings.Index(s, "."); i >= 0 {
		s = s[:i]
	}
	n, err := strconv.Atoi(s)
	if err != nil || n < 0 {
		return -1
	}
	return n
}
