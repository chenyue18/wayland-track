package procinfo

import (
	"os"
	"strconv"
	"strings"
	"time"
)

type Info struct {
	Pid     int
	Uid     int
	Gid     int
	Comm    string
	Exe     string
	Cmdline string
	FirstSeen time.Time
}

func Read(pid int, uid, gid int) Info {
	info := Info{Pid: pid, Uid: uid, Gid: gid, FirstSeen: time.Now()}
	base := "/proc/" + strconv.Itoa(pid)
	if b, err := os.ReadFile(base + "/comm"); err == nil {
		info.Comm = strings.TrimSpace(string(b))
	}
	if b, err := os.ReadFile(base + "/cmdline"); err == nil {
		info.Cmdline = strings.ReplaceAll(strings.TrimRight(string(b), "\x00"), "\x00", " ")
	}
	if s, err := os.Readlink(base + "/exe"); err == nil {
		info.Exe = s
	} else if s, err := os.Readlink(base + "/exe (deleted)"); err == nil {
		info.Exe = s
	}
	if info.Comm == "" {
		info.Comm = "unknown"
	}
	return info
}

func Alive(pid int) bool {
	b, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/stat")
	if err != nil {
		return false
	}
	s := b2s(b)
	idx := strings.LastIndexByte(s, ')')
	if idx < 0 || idx+2 >= len(s) {
		return false
	}
	state := strings.TrimSpace(s[idx+1:])[:1]
	return state != "Z" && state != "X"
}

func b2s(b []byte) string { return string(b) }
