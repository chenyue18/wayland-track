package daemon

import (
	"encoding/binary"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func wlU32(v uint32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, v)
	return b
}

func wlStr(s string) []byte {
	n := len(s) + 1
	b := make([]byte, 4+((n+3)&^3))
	binary.LittleEndian.PutUint32(b[0:4], uint32(n))
	copy(b[4:], s)
	b[4+len(s)] = 0
	return b
}

func wlMsg(obj uint32, opcode uint16, payload []byte) []byte {
	b := make([]byte, 8+len(payload))
	binary.LittleEndian.PutUint32(b[0:4], obj)
	binary.LittleEndian.PutUint16(b[4:6], opcode)
	binary.LittleEndian.PutUint16(b[6:8], uint16(8+len(payload)))
	copy(b[8:], payload)
	return b
}

func concat(bs ...[]byte) []byte {
	var out []byte
	for _, b := range bs {
		out = append(out, b...)
	}
	return out
}

func TestDaemonIntegration(t *testing.T) {
	tmp := t.TempDir()
	runtimeDir := filepath.Join(tmp, "run")
	reportDir := filepath.Join(tmp, "reports")
	_ = os.MkdirAll(runtimeDir, 0o755)

	realPath := filepath.Join(runtimeDir, "wayland-0")
	srvLn, err := net.ListenUnix("unix", &net.UnixAddr{Name: realPath, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	defer srvLn.Close()

	received := make(chan []byte, 4)
	go func() {
		c, err := srvLn.AcceptUnix()
		if err != nil {
			return
		}
		defer c.Close()
		buf := make([]byte, 8192)
		n, _ := c.Read(buf)
		received <- append([]byte{}, buf[:n]...)
		resp := concat(
			wlMsg(2, 0, concat(wlU32(1), wlStr("wl_compositor"), wlU32(5))),
			wlMsg(1, 1, wlU32(9)),
		)
		c.Write(resp)
	}()

	opts := Options{
		RuntimeDir: runtimeDir,
		ReportDir:  reportDir,
		NoX11:      true,
		Format:     "both",
		MaxMsgs:    100,
		LogOutput:  io.Discard,
	}
	d, err := New(opts)
	if err != nil {
		t.Fatal(err)
	}
	errCh := make(chan error, 1)
	go func() { errCh <- d.Run() }()

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(realPath + ".wt-real"); err == nil {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if _, err := os.Stat(realPath + ".wt-real"); err != nil {
		t.Fatal("守护进程未完成接管")
	}

	cl, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: realPath, Net: "unix"})
	if err != nil {
		t.Fatalf("客户端连接代理失败: %v", err)
	}
	if _, err := cl.Write(wlMsg(1, 1, wlU32(2))); err != nil {
		t.Fatal(err)
	}

	got := <-received
	if len(got) < 12 {
		t.Fatalf("服务端收到的数据不完整: %d 字节", len(got))
	}
	if !strings.Contains(string(got), "\x02\x00\x00\x00") && binary.LittleEndian.Uint32(got[8:12]) != 2 {
		t.Logf("服务端收到: %x", got)
	}

	buf := make([]byte, 4096)
	_ = cl.SetReadDeadline(time.Now().Add(3 * time.Second))
	n, err := cl.Read(buf)
	if err != nil || n == 0 {
		t.Fatalf("客户端未收到服务端回复: n=%d err=%v", n, err)
	}
	cl.Close()

	time.Sleep(300 * time.Millisecond)
	d.FinalizeNow()
	d.Stop()
	if err := <-errCh; err != nil {
		t.Fatalf("daemon 返回错误: %v", err)
	}

	mds, _ := filepath.Glob(filepath.Join(reportDir, "*.md"))
	jsons, _ := filepath.Glob(filepath.Join(reportDir, "*.json"))
	if len(mds) == 0 || len(jsons) == 0 {
		t.Fatalf("报告未生成: md=%v json=%v", mds, jsons)
	}
	b, _ := os.ReadFile(mds[0])
	content := string(b)
	for _, want := range []string{"get_registry", "wl_compositor", "global", "delete_id"} {
		if !strings.Contains(content, want) {
			t.Errorf("报告缺少 %q:\n%s", want, content)
		}
	}
}
