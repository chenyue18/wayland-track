package report

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"wayland-track/internal/procinfo"
)

type conn struct {
	id       uint64
	pid      int
	proto    string
	socket   string
	start    time.Time
	end      time.Time
	closed   bool
	msgs     []Msg
	count    int
	logged   int
	stats    map[string]*IfaceStat
	parser   Parser
	dropped  int
}

type session struct {
	pid       int
	app       procinfo.Info
	conns     []*conn
	first     time.Time
	last      time.Time
	openConns int
}

type Tracker struct {
	mu         sync.Mutex
	dir        string
	format     string
	maxMsgs    int
	sessions   map[int]*session
	conns      map[uint64]*conn
	nextConnID uint64
	nextConnN  int
	log        *log.Logger
}

var reSan = regexp.MustCompile(`[^a-zA-Z0-9._-]+`)

func NewTracker(dir, format string, maxMsgs int, logger *log.Logger) (*Tracker, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	if maxMsgs <= 0 {
		maxMsgs = 50000
	}
	return &Tracker{
		dir:        dir,
		format:     format,
		maxMsgs:    maxMsgs,
		sessions:   map[int]*session{},
		conns:      map[uint64]*conn{},
		nextConnID: 1,
		log:        logger,
	}, nil
}

func (t *Tracker) OpenConn(pid int, proto, socket string, app procinfo.Info, p Parser) uint64 {
	t.mu.Lock()
	defer t.mu.Unlock()
	now := time.Now()
	s, ok := t.sessions[pid]
	if !ok {
		s = &session{pid: pid, app: app, first: now}
		t.sessions[pid] = s
	}
	s.last = now
	t.nextConnN++
	c := &conn{
		id:     t.nextConnID,
		pid:    pid,
		proto:  proto,
		socket: socket,
		start:  now,
		stats:  map[string]*IfaceStat{},
		parser: p,
	}
	t.nextConnID++
	s.conns = append(s.conns, c)
	s.openConns++
	t.conns[c.id] = c
	if t.log != nil {
		t.log.Printf("连接 #%d: pid=%d %s [%s] via %s", c.id, pid, app.Comm, proto, socket)
	}
	return c.id
}

func (t *Tracker) AddMsgs(connID uint64, msgs []Msg) {
	if len(msgs) == 0 {
		return
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	c, ok := t.conns[connID]
	if !ok {
		return
	}
	now := time.Now()
	if s, ok := t.sessions[c.pid]; ok {
		s.last = now
	}
	for i := range msgs {
		m := &msgs[i]
		c.count++
		st := c.stats[m.Iface]
		if st == nil {
			st = &IfaceStat{}
			c.stats[m.Iface] = st
		}
		if m.Dir == "C→S" {
			st.Sent++
		} else {
			st.Recv++
		}
		if c.logged < t.maxMsgs {
			c.msgs = append(c.msgs, *m)
			c.logged++
		} else {
			c.dropped++
		}
	}
}

func (t *Tracker) CloseConn(connID uint64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	c, ok := t.conns[connID]
	if !ok {
		return
	}
	now := time.Now()
	c.closed = true
	c.end = now
	if s, ok := t.sessions[c.pid]; ok {
		s.openConns--
		s.last = now
		if s.openConns <= 0 && !procinfo.Alive(c.pid) {
			t.finalizeLocked(c.pid)
		}
	}
	delete(t.conns, connID)
	if t.log != nil {
		t.log.Printf("连接 #%d 关闭: pid=%d [%s] 共 %d 条消息", connID, c.pid, c.proto, c.count)
	}
}

func (t *Tracker) Sweep() {
	t.mu.Lock()
	defer t.mu.Unlock()
	for pid, s := range t.sessions {
		if !procinfo.Alive(pid) {
			t.finalizeLocked(pid)
		} else {
			_ = s
		}
	}
}

func (t *Tracker) FinalizeAll() {
	t.mu.Lock()
	defer t.mu.Unlock()
	pids := make([]int, 0, len(t.sessions))
	for pid := range t.sessions {
		pids = append(pids, pid)
	}
	sort.Ints(pids)
	for _, pid := range pids {
		t.finalizeLocked(pid)
	}
}

func (t *Tracker) finalizeLocked(pid int) {
	s, ok := t.sessions[pid]
	if !ok {
		return
	}
	delete(t.sessions, pid)

	rep := t.buildReportLocked(s)
	path := t.writeReport(rep)
	if t.log != nil {
		t.log.Printf("已生成报告: %s", path)
	}
}

func (t *Tracker) buildReportLocked(s *session) *Report {
	rep := &Report{
		App: AppInfo{
			Pid:     s.app.Pid,
			Comm:    s.app.Comm,
			Exe:     s.app.Exe,
			Cmdline: s.app.Cmdline,
			Uid:     s.app.Uid,
			Gid:     s.app.Gid,
		},
		GeneratedAt: time.Now(),
		FirstSeen:   s.first,
	}
	for _, c := range s.conns {
		cr := ConnReport{
			ID:        int(c.id),
			Proto:     c.proto,
			Socket:    c.socket,
			Start:     c.start,
			MsgCount:  c.count,
			Logged:    c.logged,
			Truncated: c.dropped > 0,
			Stats:     c.stats,
			Msgs:      c.msgs,
		}
		if c.closed {
			cr.End = c.end
		}
		if c.parser != nil {
			cr.Meta = c.parser.Summary()
		}
		rep.Connections = append(rep.Connections, cr)
	}
	return rep
}

func (t *Tracker) writeReport(rep *Report) string {
	base := fmt.Sprintf("%s_pid%d_%s", rep.FirstSeen.Format("20060102-150405"), rep.App.Pid, sanitize(rep.App.Comm))
	var paths []string
	if t.format == "json" || t.format == "both" {
		p := filepath.Join(t.dir, base+".json")
		if err := writeJSON(p, rep); err != nil {
			if t.log != nil {
				t.log.Printf("写 JSON 报告失败: %v", err)
			}
		} else {
			paths = append(paths, p)
		}
	}
	if t.format == "md" || t.format == "both" || t.format == "" {
		p := filepath.Join(t.dir, base+".md")
		if err := writeMarkdown(p, rep); err != nil {
			if t.log != nil {
				t.log.Printf("写 Markdown 报告失败: %v", err)
			}
		} else {
			paths = append(paths, p)
		}
	}
	if len(paths) == 0 {
		return "(报告写入失败)"
	}
	return strings.Join(paths, " + ")
}

func sanitize(s string) string {
	s = reSan.ReplaceAllString(s, "_")
	s = strings.Trim(s, "_.-")
	if len(s) > 48 {
		s = s[:48]
	}
	if s == "" {
		return "unknown"
	}
	return s
}

func ifacesSorted(stats map[string]*IfaceStat) []string {
	names := make([]string, 0, len(stats))
	for n := range stats {
		names = append(names, n)
	}
	sort.Slice(names, func(i, j int) bool {
		return stats[names[i]].Sent+stats[names[i]].Recv > stats[names[j]].Sent+stats[names[j]].Recv
	})
	return names
}

func fmtTime(t time.Time) string {
	if t.IsZero() {
		return "-"
	}
	return t.Format("2006-01-02 15:04:05.000")
}
