package report

import (
	"time"
)

type Msg struct {
	Time   time.Time `json:"time"`
	Dir    string    `json:"dir"`
	Iface  string    `json:"interface"`
	Name   string    `json:"name"`
	Object string    `json:"object,omitempty"`
	Detail string    `json:"detail,omitempty"`
	Bytes  int       `json:"bytes"`
}

type IfaceStat struct {
	Sent int `json:"sent"`
	Recv int `json:"recv"`
}

type ConnReport struct {
	ID       int                `json:"id"`
	Proto    string             `json:"protocol"`
	Socket   string             `json:"socket"`
	Start    time.Time          `json:"start"`
	End      time.Time          `json:"end,omitempty"`
	MsgCount int                `json:"message_count"`
	Logged   int                `json:"logged_messages"`
	Truncated bool              `json:"truncated,omitempty"`
	Stats    map[string]*IfaceStat `json:"interface_stats"`
	Meta     map[string]string  `json:"meta"`
	Msgs     []Msg              `json:"messages"`
}

type Report struct {
	App          AppInfo       `json:"app"`
	GeneratedAt  time.Time     `json:"generated_at"`
	FirstSeen    time.Time     `json:"first_seen"`
	Connections  []ConnReport  `json:"connections"`
}

type AppInfo struct {
	Pid     int    `json:"pid"`
	Comm    string `json:"comm"`
	Exe     string `json:"exe,omitempty"`
	Cmdline string `json:"cmdline,omitempty"`
	Uid     int    `json:"uid"`
	Gid     int    `json:"gid"`
}

type Parser interface {
	Feed(dir int, data []byte, fdCount int, t time.Time) ([]Msg, int)
	Summary() map[string]string
}
