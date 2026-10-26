package x11

import (
	"encoding/binary"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"wayland-track/internal/report"
)

const (
	DirC2S = 0
	DirS2C = 1
)

const (
	phaseSetup = 0
	phaseStream = 1
	phaseDead  = 2
)

type extEvent struct {
	first byte
	name  string
}

type Parser struct {
	mu        sync.Mutex
	bo        binary.ByteOrder
	buf       [2][]byte
	phase     [2]int
	extName   map[byte]string
	extEvents []extEvent
	pending   map[uint16]string
	atoms     map[uint32]string
	seq       uint16
	vendor    string
	major     int
	minor     int
	release   uint32
	extList   []string
	parseErr  [2]bool
}

func New() *Parser {
	return &Parser{
		bo:      binary.LittleEndian,
		extName: map[byte]string{},
		pending: map[uint16]string{},
		atoms:   map[uint32]string{},
	}
}

func pad4(n int) int { return (n + 3) &^ 3 }

func (p *Parser) u16(b []byte, off int) (uint16, bool) {
	if off+2 > len(b) {
		return 0, false
	}
	return p.bo.Uint16(b[off : off+2]), true
}

func (p *Parser) u32(b []byte, off int) (uint32, bool) {
	if off+4 > len(b) {
		return 0, false
	}
	return p.bo.Uint32(b[off : off+4]), true
}

func (p *Parser) Feed(dir int, data []byte, fdCount int, t time.Time) ([]report.Msg, int) {
	p.mu.Lock()
	defer p.mu.Unlock()

	p.buf[dir] = append(p.buf[dir], data...)
	var msgs []report.Msg
	totalBytes := 0
	dirName := "C→S"
	if dir == DirS2C {
		dirName = "S→C"
	}

	for !p.parseErr[dir] && p.phase[dir] != phaseDead {
		b := p.buf[dir]
		var consumed int
		var m *report.Msg
		var ok bool
		if p.phase[dir] == phaseSetup {
			consumed, m, ok = p.stepSetup(dir, dirName, b, t)
		} else if dir == DirC2S {
			consumed, m, ok = p.stepRequest(dirName, b, fdCount, t)
		} else {
			consumed, m, ok = p.stepReply(dirName, b, t)
		}
		if !ok {
			break
		}
		if m != nil {
			msgs = append(msgs, *m)
			totalBytes += consumed
		}
	}
	if len(p.buf[dir]) == 0 {
		p.buf[dir] = nil
	}
	return msgs, totalBytes
}

func (p *Parser) stepSetup(dir int, dirName string, b []byte, t time.Time) (int, *report.Msg, bool) {
	if dir == DirC2S {
		if len(b) < 12 {
			return 0, nil, false
		}
		order := b[0]
		if order == 'l' {
			p.bo = binary.LittleEndian
		} else if order == 'B' {
			p.bo = binary.BigEndian
		} else {
			p.parseErr[dir] = true
			return 0, nil, false
		}
		nameLen, _ := p.u16(b, 6)
		dataLen, _ := p.u16(b, 8)
		need := 12 + pad4(int(nameLen)) + pad4(int(dataLen))
		if len(b) < need {
			return 0, nil, false
		}
		authName := ""
		if int(nameLen) > 0 {
			authName = string(b[12 : 12+int(nameLen)])
		}
		p.phase[dir] = phaseStream
		p.seq = 1
		m := &report.Msg{
			Time:   t,
			Dir:    dirName,
			Iface:  "x11",
			Name:   "Setup(握手)",
			Object: fmt.Sprintf("byte-order=%c", order),
			Detail: fmt.Sprintf("认证方式=%q", authName),
			Bytes:  need,
		}
		p.buf[dir] = b[need:]
		return need, m, true
	}

	if len(b) < 8 {
		return 0, nil, false
	}
	success := b[0]
	addLen, _ := p.u16(b, 6)
	total := 8 + int(addLen)*4
	switch success {
	case 0:
		if len(b) < total {
			return 0, nil, false
		}
		reason := ""
		if len(b) >= 2 {
			rl := int(b[1])
			if 32+rl <= len(b) {
				reason = string(b[32 : 32+rl])
			}
		}
		p.phase[dir] = phaseDead
		m := &report.Msg{Time: t, Dir: dirName, Iface: "x11", Name: "Setup-Fail(连接被拒)", Detail: "原因=" + reason, Bytes: total}
		p.buf[dir] = b[total:]
		return total, m, true
	case 2:
		if len(b) < total {
			return 0, nil, false
		}
		m := &report.Msg{Time: t, Dir: dirName, Iface: "x11", Name: "Setup-RequireAuth", Bytes: total}
		p.buf[dir] = b[total:]
		return total, m, true
	case 1:
		if len(b) < total {
			return 0, nil, false
		}
		if ma, ok := p.u16(b, 2); ok {
			p.major = int(ma)
		}
		if mi, ok := p.u16(b, 4); ok {
			p.minor = int(mi)
		}
		if rel, ok := p.u32(b, 8); ok {
			p.release = rel
		}
		if vl, ok := p.u16(b, 24); ok {
			if 40+int(vl) <= len(b) {
				p.vendor = strings.TrimRight(string(b[40:40+int(vl)]), "\x00")
			}
		}
		p.phase[dir] = phaseStream
		m := &report.Msg{
			Time:   t,
			Dir:    dirName,
			Iface:  "x11",
			Name:   "Setup-Success",
			Detail: fmt.Sprintf("vendor=%q 协议=%d.%d release=%d", p.vendor, p.major, p.minor, p.release),
			Bytes:  total,
		}
		p.buf[dir] = b[total:]
		return total, m, true
	default:
		p.parseErr[dir] = true
		return 0, nil, false
	}
}

func (p *Parser) stepRequest(dirName string, b []byte, fdCount int, t time.Time) (int, *report.Msg, bool) {
	if len(b) < 4 {
		return 0, nil, false
	}
	opcode := b[0]
	realOpcode := opcode
	total := 0
	if opcode == 254 {
		if len(b) < 8 {
			return 0, nil, false
		}
		realOpcode = b[1]
		ext := p.bo.Uint32(b[4:8])
		total = int(ext) * 4
		if total < 8 || total > len(b) {
			if total > len(b) {
				return 0, nil, false
			}
			p.parseErr[DirC2S] = true
			return 0, nil, false
		}
	} else {
		l, _ := p.u16(b, 2)
		total = int(l) * 4
		if total < 4 || total > len(b) {
			if total > len(b) {
				return 0, nil, false
			}
			p.parseErr[DirC2S] = true
			return 0, nil, false
		}
	}
	if realOpcode == 0 {
		p.parseErr[DirC2S] = true
		return 0, nil, false
	}

	iface := "core"
	name := fmt.Sprintf("Request-%d", realOpcode)
	detail := fmt.Sprintf("%d 字节", total)
	if realOpcode >= 128 {
		if en, ok := p.extName[realOpcode]; ok {
			iface = en
			name = fmt.Sprintf("%s.%s", en, fmt.Sprintf("minor-%d", b[1]))
		} else {
			iface = fmt.Sprintf("ext#%d", realOpcode)
			name = fmt.Sprintf("ext#%d(minor=%d)", realOpcode, b[1])
		}
	} else {
		if n, ok := reqNames[realOpcode]; ok {
			name = n
		}
		detail = p.decodeRequest(realOpcode, b[:total])
	}
	if fdCount > 0 {
		detail += fmt.Sprintf(" (+%d fd)", fdCount)
	}

	seq := p.seq
	p.seq++

	switch realOpcode {
	case 98:
		if nl, ok := p.u16(b, 4); ok && 8+int(nl) <= total {
			p.pending[seq] = string(b[8 : 8+int(nl)])
		}
	case 16:
		if nl, ok := p.u16(b, 4); ok && 6+int(nl) <= total {
			p.pending[seq] = "atom:" + string(b[6:6+int(nl)])
		}
	}

	m := &report.Msg{
		Time:   t,
		Dir:    dirName,
		Iface:  iface,
		Name:   name,
		Detail: detail,
		Bytes:  total,
	}
	p.buf[DirC2S] = b[total:]
	return total, m, true
}

func (p *Parser) atomName(id uint32) string {
	if n, ok := p.atoms[id]; ok {
		return fmt.Sprintf("%s(0x%x)", n, id)
	}
	return fmt.Sprintf("0x%x", id)
}

func (p *Parser) decodeRequest(op uint8, b []byte) string {
	switch op {
	case 1:
		w, _ := p.u32(b, 4)
		par, _ := p.u32(b, 8)
		depth := ""
		if len(b) > 1 {
			depth = fmt.Sprintf(" depth=%d", b[1])
		}
		return fmt.Sprintf("win=0x%x parent=0x%x%s", w, par, depth)
	case 2, 3, 4, 5, 8, 9, 10, 11, 14, 15, 17, 21, 25:
		w, ok := p.u32(b, 4)
		if ok {
			return fmt.Sprintf("win=0x%x", w)
		}
	case 12:
		w, _ := p.u32(b, 4)
		mask, _ := p.u16(b, 8)
		return fmt.Sprintf("win=0x%x mask=0x%04x", w, mask)
	case 16:
		if nl, ok := p.u16(b, 4); ok && 6+int(nl) <= len(b) {
			return fmt.Sprintf("name=%q only-if-exists=%d", string(b[6:6+int(nl)]), b[1])
		}
	case 18:
		w, _ := p.u32(b, 4)
		prop, _ := p.u32(b, 8)
		typ, _ := p.u32(b, 12)
		n, _ := p.u32(b, 20)
		if len(b) > 16 {
			return fmt.Sprintf("win=0x%x prop=%s type=%s fmt=%d n=%d", w, p.atomName(prop), p.atomName(typ), b[16], n)
		}
	case 19, 20:
		w, _ := p.u32(b, 4)
		prop, _ := p.u32(b, 8)
		return fmt.Sprintf("win=0x%x prop=%s", w, p.atomName(prop))
	case 22:
		owner, _ := p.u32(b, 4)
		sel, _ := p.u32(b, 8)
		return fmt.Sprintf("owner=0x%x selection=%s", owner, p.atomName(sel))
	case 24:
		req, _ := p.u32(b, 4)
		sel, _ := p.u32(b, 8)
		tgt, _ := p.u32(b, 12)
		prop, _ := p.u32(b, 16)
		return fmt.Sprintf("requestor=0x%x selection=%s target=%s property=%s", req, p.atomName(sel), p.atomName(tgt), p.atomName(prop))
	case 45:
		fid, _ := p.u32(b, 4)
		nl, _ := p.u16(b, 8)
		name := ""
		if 10+int(nl) <= len(b) {
			name = string(b[10 : 10+int(nl)])
		}
		return fmt.Sprintf("font=0x%x name=%q", fid, name)
	case 53:
		pid, _ := p.u32(b, 4)
		win, _ := p.u32(b, 8)
		return fmt.Sprintf("pixmap=0x%x drawable=0x%x", pid, win)
	case 54, 60, 95:
		id, _ := p.u32(b, 4)
		return fmt.Sprintf("id=0x%x", id)
	case 55, 56:
		id, _ := p.u32(b, 4)
		win, _ := p.u32(b, 8)
		return fmt.Sprintf("gc=0x%x drawable=0x%x", id, win)
	case 47:
		id, _ := p.u32(b, 4)
		return fmt.Sprintf("drawable=0x%x", id)
	case 39:
		w, _ := p.u32(b, 4)
		return fmt.Sprintf("win=0x%x", w)
	case 72:
		win, _ := p.u32(b, 4)
		gc, _ := p.u32(b, 8)
		w, _ := p.u16(b, 16)
		h, _ := p.u16(b, 18)
		return fmt.Sprintf("drawable=0x%x gc=0x%x %dx%d", win, gc, w, h)
	case 73:
		win, _ := p.u32(b, 4)
		gc, _ := p.u32(b, 8)
		w, _ := p.u16(b, 16)
		h, _ := p.u16(b, 18)
		return fmt.Sprintf("drawable=0x%x gc=0x%x %dx%d", win, gc, w, h)
	case 74, 75, 76, 77:
		win, _ := p.u32(b, 4)
		gc, _ := p.u32(b, 8)
		return fmt.Sprintf("win=0x%x gc=0x%x", win, gc)
	case 93:
		cid, _ := p.u32(b, 4)
		return fmt.Sprintf("cursor=0x%x", cid)
	case 98:
		if nl, ok := p.u16(b, 4); ok && 8+int(nl) <= len(b) {
			return fmt.Sprintf("name=%q", string(b[8:8+int(nl)]))
		}
	case 127:
		return "空操作"
	}
	return fmt.Sprintf("%d 字节", len(b))
}

func (p *Parser) stepReply(dirName string, b []byte, t time.Time) (int, *report.Msg, bool) {
	if len(b) < 1 {
		return 0, nil, false
	}
	first := b[0]
	switch {
	case first == 0:
		if len(b) < 32 {
			return 0, nil, false
		}
		code := b[1]
		seq := p.bo.Uint16(b[2:4])
		name := fmt.Sprintf("Error-%d", code)
		if n, ok := errNames[code]; ok {
			name = "Error:" + n
		}
		if pending, ok := p.pending[seq]; ok {
			delete(p.pending, seq)
			name += fmt.Sprintf(" [%s]", pending)
		}
		m := &report.Msg{Time: t, Dir: dirName, Iface: "x11", Name: name, Detail: fmt.Sprintf("seq=%d", seq), Bytes: 32}
		p.buf[DirS2C] = b[32:]
		return 32, m, true
	case first == 1:
		if len(b) < 8 {
			return 0, nil, false
		}
		addLen := p.bo.Uint32(b[4:8])
		total := 32 + int(addLen)*4
		if int(addLen) > 0x100000 || total > len(b) {
			if total > len(b) {
				return 0, nil, false
			}
			p.parseErr[DirS2C] = true
			return 0, nil, false
		}
		seq := p.bo.Uint16(b[2:4])
		name := "Reply"
		detail := fmt.Sprintf("seq=%d %d 字节", seq, total)
		if pending, ok := p.pending[seq]; ok {
			delete(p.pending, seq)
			if strings.HasPrefix(pending, "atom:") {
				atomName := strings.TrimPrefix(pending, "atom:")
				if aid, ok2 := p.u32(b, 8); ok2 {
					p.atoms[aid] = atomName
					detail = fmt.Sprintf("seq=%d atom %q = 0x%x", seq, atomName, aid)
					name = "InternAtom-Reply"
				}
			} else {
				present := b[8]
				if present == 1 {
					major := b[9]
					firstEvent := b[10]
					p.extName[major] = pending
					p.extEvents = append(p.extEvents, extEvent{first: firstEvent, name: pending})
					p.extList = append(p.extList, fmt.Sprintf("%s(op=%d,evt=%d)", pending, major, firstEvent))
					name = "QueryExtension-Reply"
					detail = fmt.Sprintf("seq=%d 扩展=%q major-opcode=%d first-event=%d", seq, pending, major, firstEvent)
				} else {
					name = "QueryExtension-Reply(不存在)"
					detail = fmt.Sprintf("seq=%d 扩展=%q 不存在", seq, pending)
				}
			}
		}
		m := &report.Msg{Time: t, Dir: dirName, Iface: "x11", Name: name, Detail: detail, Bytes: total}
		p.buf[DirS2C] = b[total:]
		return total, m, true
	case first == 35:
		if len(b) < 8 {
			return 0, nil, false
		}
		addLen := p.bo.Uint32(b[4:8])
		total := 32 + int(addLen)*4
		if int(addLen) > 0x100000 || total > len(b) {
			if total > len(b) {
				return 0, nil, false
			}
			p.parseErr[DirS2C] = true
			return 0, nil, false
		}
		ext := b[1]
		evtype := p.bo.Uint16(b[8:10])
		extn := fmt.Sprintf("ext#%d", ext)
		if n, ok := p.extName[ext]; ok {
			extn = n
		}
		m := &report.Msg{
			Time:  t,
			Dir:   dirName,
			Iface: extn,
			Name:  fmt.Sprintf("GenericEvent(type=%d)", evtype),
			Bytes: total,
		}
		p.buf[DirS2C] = b[total:]
		return total, m, true
	case first >= 2 && first <= 34:
		if len(b) < 32 {
			return 0, nil, false
		}
		name := fmt.Sprintf("Event-%d", first)
		if n, ok := evtNames[first]; ok {
			name = n
		}
		detail := ""
		if first != 11 {
			seq := p.bo.Uint16(b[2:4])
			detail = fmt.Sprintf("seq=%d", seq)
		}
		m := &report.Msg{Time: t, Dir: dirName, Iface: "x11", Name: name, Detail: detail, Bytes: 32}
		p.buf[DirS2C] = b[32:]
		return 32, m, true
	case first >= 64:
		if len(b) < 32 {
			return 0, nil, false
		}
		name := fmt.Sprintf("ExtEvent-%d", first)
		if n := p.extEventName(first); n != "" {
			name = n + "-Event"
		}
		seq := p.bo.Uint16(b[2:4])
		m := &report.Msg{Time: t, Dir: dirName, Iface: "x11", Name: name, Detail: fmt.Sprintf("seq=%d", seq), Bytes: 32}
		p.buf[DirS2C] = b[32:]
		return 32, m, true
	default:
		p.parseErr[DirS2C] = true
		return 0, nil, false
	}
}

func (p *Parser) extEventName(code byte) string {
	best := ""
	var bestFirst byte
	for _, e := range p.extEvents {
		if code >= e.first && (best == "" || e.first >= bestFirst) {
			best = e.name
			bestFirst = e.first
		}
	}
	return best
}

func (p *Parser) Summary() map[string]string {
	p.mu.Lock()
	defer p.mu.Unlock()
	m := map[string]string{}
	if p.vendor != "" {
		m["server_vendor"] = p.vendor
		m["protocol_version"] = fmt.Sprintf("%d.%d", p.major, p.minor)
		m["server_release"] = fmt.Sprintf("%d", p.release)
	}
	if len(p.extList) > 0 {
		sort.Strings(p.extList)
		m["x11_extensions"] = strings.Join(p.extList, ", ")
	}
	if len(p.atoms) > 0 {
		m["known_atoms"] = fmt.Sprintf("%d 个（通过 InternAtom 记录）", len(p.atoms))
	}
	for d := range p.parseErr {
		if p.parseErr[d] {
			m["parse_error"] = "X11 流解析失败，出错位置之后的数据未解码（转发不受影响）"
			break
		}
	}
	return m
}
