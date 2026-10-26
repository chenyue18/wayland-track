package takeover

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"syscall"
)

type Target struct {
	Proto   string `json:"proto"`
	Proxy   string `json:"proxy"`
	Real    string `json:"real"`
	Display string `json:"display,omitempty"`
	Abstract bool  `json:"abstract,omitempty"`
}

type State struct {
	Pid       int      `json:"daemon_pid"`
	ReportDir string   `json:"report_dir"`
	Targets   []Target `json:"takeovers"`
}

func StatePath(runtimeDir string) string {
	return filepath.Join(runtimeDir, "wayland-track", "state.json")
}

func LoadState(runtimeDir string) *State {
	b, err := os.ReadFile(StatePath(runtimeDir))
	if err != nil {
		return nil
	}
	var st State
	if json.Unmarshal(b, &st) != nil {
		return nil
	}
	return &st
}

func SaveState(runtimeDir string, st *State) error {
	p := StatePath(runtimeDir)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	b, _ := json.MarshalIndent(st, "", "  ")
	return os.WriteFile(p, b, 0o600)
}

func RemoveState(runtimeDir string) {
	os.Remove(StatePath(runtimeDir))
}

func Discover(runtimeDir string, wantWayland, wantX11 bool) []Target {
	var out []Target
	if wantWayland && runtimeDir != "" {
		entries, _ := os.ReadDir(runtimeDir)
		for _, e := range entries {
			name := e.Name()
			if !strings.HasPrefix(name, "wayland-") {
				continue
			}
			if strings.Contains(name, "track") || strings.HasSuffix(name, ".wt-real") {
				continue
			}
			full := filepath.Join(runtimeDir, name)
			if isUnixSocket(full) {
				out = append(out, Target{Proto: "Wayland", Proxy: full, Real: full + ".wt-real", Display: name})
			}
		}
	}
	if wantX11 {
		entries, _ := os.ReadDir("/tmp/.X11-unix")
		for _, e := range entries {
			name := e.Name()
			if len(name) < 2 || name[0] != 'X' {
				continue
			}
			if !isNum(name[1:]) {
				continue
			}
			if strings.HasSuffix(name, ".wt-real") {
				continue
			}
			full := filepath.Join("/tmp/.X11-unix", name)
			if isUnixSocket(full) {
				out = append(out, Target{Proto: "X11", Proxy: full, Real: full + ".wt-real", Display: name})
			}
		}
	}
	return out
}

func isNum(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return len(s) > 0
}

func isUnixSocket(path string) bool {
	fi, err := os.Stat(path)
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeSocket != 0
}

func Takeover(t Target) (net.Listener, error) {
	if _, err := os.Stat(t.Real); err == nil {
		return nil, fmt.Errorf("残留的真实 socket %s 已存在，请先执行 wayland-track restore", t.Real)
	}
	if err := os.Rename(t.Proxy, t.Real); err != nil {
		return nil, fmt.Errorf("重命名 %s 失败: %w", t.Proxy, err)
	}
	ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: t.Proxy, Net: "unix"})
	if err != nil {
		os.Rename(t.Real, t.Proxy)
		return nil, fmt.Errorf("在 %s 上创建代理监听失败: %w", t.Proxy, err)
	}
	_ = os.Chmod(t.Proxy, 0o777)
	return ln, nil
}

func ListenPair(t Target) ([]net.Listener, error) {
	ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: t.Proxy, Net: "unix"})
	if err != nil {
		return nil, err
	}
	listeners := []net.Listener{ln}
	if t.Abstract {
		lnA, err := net.ListenUnix("unix", &net.UnixAddr{Name: "@" + t.Proxy, Net: "unix"})
		if err != nil {
			ln.Close()
			return nil, err
		}
		listeners = append(listeners, lnA)
	}
	return listeners, nil
}

func Restore(runtimeDir string) ([]string, error) {
	st := LoadState(runtimeDir)
	if st == nil {
		return recoverScan(runtimeDir)
	}
	var msgs []string
	for _, t := range st.Targets {
		if t.Abstract {
			continue
		}
		if _, err := os.Stat(t.Real); err != nil {
			msgs = append(msgs, fmt.Sprintf("%s: 无需恢复（真实 socket 已在原位）", t.Proxy))
			continue
		}
		if err := os.Rename(t.Real, t.Proxy); err != nil {
			msgs = append(msgs, fmt.Sprintf("恢复 %s 失败: %v", t.Proxy, err))
		} else {
			msgs = append(msgs, fmt.Sprintf("已恢复 %s", t.Proxy))
		}
	}
	RemoveState(runtimeDir)
	left, _ := recoverScan(runtimeDir)
	msgs = append(msgs, left...)
	return msgs, nil
}

func recoverScan(runtimeDir string) ([]string, error) {
	dirs := []string{runtimeDir, "/tmp/.X11-unix"}
	var msgs []string
	found := false
	for _, dir := range dirs {
		entries, _ := os.ReadDir(dir)
		for _, e := range entries {
			if !strings.HasSuffix(e.Name(), ".wt-real") {
				continue
			}
			found = true
			real := filepath.Join(dir, e.Name())
			proxy := strings.TrimSuffix(real, ".wt-real")
			if err := os.Rename(real, proxy); err != nil {
				msgs = append(msgs, fmt.Sprintf("恢复 %s 失败: %v", proxy, err))
			} else {
				msgs = append(msgs, fmt.Sprintf("已恢复(扫描) %s", proxy))
			}
		}
	}
	if !found {
		return nil, fmt.Errorf("没有找到接管状态文件，也无需恢复")
	}
	return msgs, nil
}

func PeerCred(c *net.UnixConn) (pid, uid, gid int, err error) {
	raw, err := c.SyscallConn()
	if err != nil {
		return 0, 0, 0, err
	}
	var ucred *syscall.Ucred
	e := raw.Control(func(fd uintptr) {
		ucred, err = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	})
	if e != nil {
		return 0, 0, 0, e
	}
	if err != nil {
		return 0, 0, 0, err
	}
	return int(ucred.Pid), int(ucred.Uid), int(ucred.Gid), nil
}
