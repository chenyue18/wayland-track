package wayland

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
	"time"

	"wayland-track/internal/protocol"
	"wayland-track/internal/report"
)

const (
	DirC2S = 0
	DirS2C = 1
)

type Parser struct {
	mu       sync.Mutex
	db       *protocol.DB
	objects  map[uint32]string
	buf      [2][]byte
	fdAvail  [2]int
	fdDebt   [2]int
	fdTotal  [2]int
	globals  map[uint32]string
	binds    []string
	parseErr [2]bool
}

func New(db *protocol.DB) *Parser {
	if db == nil {
		db = protocol.New()
	}
	return &Parser{
		db:      db,
		objects: map[uint32]string{1: "wl_display"},
		globals: map[uint32]string{},
	}
}

func objName(objects map[uint32]string, id uint32) string {
	if n, ok := objects[id]; ok {
		return fmt.Sprintf("%s@%d", n, id)
	}
	if id >= 0xff000000 {
		return fmt.Sprintf("server-object@%d", id)
	}
	return fmt.Sprintf("object@%d", id)
}

type decoded struct {
	name   string
	detail string
}

func (p *Parser) Feed(dir int, data []byte, fdCount int, t time.Time) ([]report.Msg, int) {
	p.mu.Lock()
	defer p.mu.Unlock()

	p.fdAvail[dir] += fdCount
	p.buf[dir] = append(p.buf[dir], data...)

	var msgs []report.Msg
	dirName := "C→S"
	if dir == DirS2C {
		dirName = "S→C"
	}
	var totalBytes int

	for !p.parseErr[dir] {
		b := p.buf[dir]
		if len(b) < 8 {
			break
		}
		objID := binary.LittleEndian.Uint32(b[0:4])
		opcode := binary.LittleEndian.Uint16(b[4:6])
		size := int(binary.LittleEndian.Uint16(b[6:8]))
		if size < 8 {
			p.parseErr[dir] = true
			break
		}
		if size > len(b) {
			break
		}
		payload := b[8:size]
		iface := p.objects[objID]
		d := p.decode(dir, iface, objID, opcode, payload)
		if iface == "" {
			iface = "unknown"
		}
		msgs = append(msgs, report.Msg{
			Time:   t,
			Dir:    dirName,
			Iface:  iface,
			Name:   d.name,
			Object: objName(p.objects, objID),
			Detail: d.detail,
			Bytes:  size,
		})
		totalBytes += size
		p.buf[dir] = b[size:]
	}
	if len(p.buf[dir]) == 0 {
		p.buf[dir] = nil
	}
	return msgs, totalBytes
}

func (p *Parser) decode(dir int, iface string, objID uint32, opcode uint16, payload []byte) decoded {
	var def *protocol.MsgDef
	if iface != "" {
		if dir == DirC2S {
			def = p.db.Request(iface, int(opcode))
		} else {
			def = p.db.Event(iface, int(opcode))
		}
	}
	if def == nil {
		return decoded{
			name:   fmt.Sprintf("opcode-%d", opcode),
			detail: rawWords(payload),
		}
	}

	rd := &reader{b: payload}
	var parts []string

	for i := range def.Args {
		a := &def.Args[i]
		v, err := p.decodeArg(dir, rd, a, iface, objID)
		if err != nil {
			parts = append(parts, fmt.Sprintf("%s=<malformed>", a.Name))
			break
		}
		parts = append(parts, v)
	}

	if name := def.Name; name == "delete_id" && dir == DirS2C && iface == "wl_display" && len(payload) >= 4 {
		delete(p.objects, binary.LittleEndian.Uint32(payload[0:4]))
	}
	if name := def.Name; name == "global" && iface == "wl_registry" && dir == DirS2C {
		r := &reader{b: payload}
		if n, err := r.u32(); err == nil {
			if s, err := r.str(); err == nil {
				p.globals[n] = s
			}
		}
	}

	detail := strings.Join(parts, ", ")
	return decoded{name: def.Name, detail: detail}
}

func (p *Parser) decodeArg(dir int, rd *reader, a *protocol.Arg, iface string, objID uint32) (string, error) {
	switch a.Type {
	case "int":
		x, err := rd.i32()
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("%s=%d", a.Name, x), nil
	case "uint":
		x, err := rd.u32()
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("%s=%d", a.Name, x), nil
	case "fixed":
		x, err := rd.i32()
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("%s=%s", a.Name, formatFixed(x)), nil
	case "string":
		s, err := rd.str()
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("%s=%q", a.Name, s), nil
	case "object":
		x, err := rd.u32()
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("%s=%s", a.Name, objName(p.objects, x)), nil
	case "new_id":
		if a.Iface == "" {
			sname, err := rd.str()
			if err != nil {
				return "", err
			}
			ver, err := rd.u32()
			if err != nil {
				return "", err
			}
			id, err := rd.u32()
			if err != nil {
				return "", err
			}
			p.objects[id] = sname
			if iface == "wl_registry" && dir == DirC2S {
				p.binds = append(p.binds, fmt.Sprintf("%s@%d (v%d)", sname, id, ver))
			}
			return fmt.Sprintf("%s=%s@%d v%d", a.Name, sname, id, ver), nil
		}
		id, err := rd.u32()
		if err != nil {
			return "", err
		}
		p.objects[id] = a.Iface
		return fmt.Sprintf("%s=%s@%d", a.Name, a.Iface, id), nil
	case "array":
		n, err := rd.u32()
		if err != nil {
			return "", err
		}
		if !rd.skipBytes(int(n)) {
			return "", errTruncated
		}
		return fmt.Sprintf("%s=<%d bytes>", a.Name, n), nil
	case "fd":
		if p.fdAvail[dir] > 0 {
			p.fdAvail[dir]--
			idx := p.fdTotal[dir]
			p.fdTotal[dir]++
			return fmt.Sprintf("%s=fd#%d", a.Name, idx), nil
		}
		p.fdDebt[dir]++
		return fmt.Sprintf("%s=fd(?)", a.Name), nil
	default:
		x, err := rd.u32()
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("%s=%d", a.Name, x), nil
	}
}

var errTruncated = fmt.Errorf("truncated")

type reader struct {
	b   []byte
	off int
}

func (r *reader) u32() (uint32, error) {
	if r.off+4 > len(r.b) {
		return 0, errTruncated
	}
	v := binary.LittleEndian.Uint32(r.b[r.off : r.off+4])
	r.off += 4
	return v, nil
}

func (r *reader) i32() (int32, error) {
	v, err := r.u32()
	return int32(v), err
}

func (r *reader) str() (string, error) {
	n, err := r.u32()
	if err != nil {
		return "", err
	}
	if int(n) > 1<<20 || r.off+int(n) > len(r.b) {
		return "", errTruncated
	}
	s := string(r.b[r.off : r.off+int(n)])
	r.off += int(n)
	r.off = (r.off + 3) &^ 3
	if i := strings.IndexByte(s, 0); i >= 0 {
		s = s[:i]
	}
	return s, nil
}

func (r *reader) skipBytes(n int) bool {
	if r.off+n > len(r.b) {
		return false
	}
	r.off += n
	r.off = (r.off + 3) &^ 3
	return true
}

func formatFixed(f int32) string {
	v := float64(f) / 256.0
	if v == math.Trunc(v) {
		return fmt.Sprintf("%d", int64(v))
	}
	return fmt.Sprintf("%.4g", v)
}

func rawWords(payload []byte) string {
	var w []string
	for i := 0; i+4 <= len(payload) && i < 64; i += 4 {
		w = append(w, fmt.Sprintf("%08x", binary.LittleEndian.Uint32(payload[i:i+4])))
	}
	if len(w) == 0 {
		return fmt.Sprintf("<%d bytes raw>", len(payload))
	}
	s := strings.Join(w, " ")
	if len(payload) > 64 {
		s += " ..."
	}
	return "raw: " + s
}

func (p *Parser) Summary() map[string]string {
	p.mu.Lock()
	defer p.mu.Unlock()
	m := map[string]string{}
	if len(p.globals) > 0 {
		names := make([]string, 0, len(p.globals))
		for _, g := range p.globals {
			names = append(names, g)
		}
		sort.Strings(names)
		m["registry_globals"] = strings.Join(names, ", ")
	}
	if len(p.binds) > 0 {
		seen := map[string]int{}
		var order []string
		for _, b := range p.binds {
			iface := b
			if i := strings.IndexByte(iface, '@'); i > 0 {
				iface = iface[:i]
			}
			if _, ok := seen[iface]; !ok {
				seen[iface] = 0
				order = append(order, iface)
			}
			seen[iface]++
		}
		parts := make([]string, 0, len(order))
		for _, iface := range order {
			parts = append(parts, fmt.Sprintf("%s×%d", iface, seen[iface]))
		}
		if len(parts) > 40 {
			m["bound_objects"] = fmt.Sprintf("%d 个不同接口: %s ...", len(parts), strings.Join(parts[:40], ", "))
		} else {
			m["bound_objects"] = fmt.Sprintf("%d 个不同接口: %s", len(parts), strings.Join(parts, ", "))
		}
	}
	for d, e := range p.parseErr {
		if e {
			m["parse_error"] = "wire 流解析失败，出错位置之后的数据未解码（转发不受影响）"
			break
		}
		_ = d
	}
	return m
}
