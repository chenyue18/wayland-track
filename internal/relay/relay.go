package relay

import (
	"fmt"
	"net"
	"os"
	"syscall"
	"time"

	"wayland-track/internal/report"
)

const (
	DirC2S = 0
	DirS2C = 1
)

func closeFds(fds []int) {
	for _, fd := range fds {
		syscall.Close(fd)
	}
}

func rightsOf(oob []byte) []int {
	if len(oob) == 0 {
		return nil
	}
	scms, _ := syscall.ParseSocketControlMessage(oob)
	var fds []int
	for i := range scms {
		rights, _ := syscall.ParseUnixRights(&scms[i])
		fds = append(fds, rights...)
	}
	return fds
}

func sendAll(dst *net.UnixConn, data []byte, fds []int) error {
	var oob []byte
	if len(fds) > 0 {
		oob = syscall.UnixRights(fds...)
	}
	n, _, err := dst.WriteMsgUnix(data, oob, nil)
	if err != nil {
		return err
	}
	if n < len(data) {
		_, err = dst.Write(data[n:])
	}
	return err
}

func Pump(src, dst *net.UnixConn, dir int, p report.Parser, tracker *report.Tracker, connID uint64, onClose func()) {
	defer onClose()
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "wayland-track: 转发线程异常(已恢复，不影响其他连接): %v\n", r)
		}
	}()
	pumpImpl(src, dst, dir, p, tracker, connID)
}

func pumpImpl(src, dst *net.UnixConn, dir int, p report.Parser, tracker *report.Tracker, connID uint64) {
	buf := make([]byte, 256<<10)
	oob := make([]byte, 16<<10)
	for {
		n, oobn, _, _, err := src.ReadMsgUnix(buf, oob)
		if n == 0 && err != nil {
			return
		}
		if n > 0 {
			fds := rightsOf(oob[:oobn])
			msgs, _ := p.Feed(dir, buf[:n], len(fds), time.Now())
			if len(msgs) > 0 {
				tracker.AddMsgs(connID, msgs)
			}
			if err := sendAll(dst, buf[:n], fds); err != nil {
				closeFds(fds)
				return
			}
			closeFds(fds)
		}
		if err != nil {
			return
		}
		if n == 0 {
			return
		}
	}
}
