package wayland

import (
	"encoding/binary"
	"strings"
	"testing"
	"time"

	"wayland-track/internal/protocol"
)

func wlMsg(obj uint32, opcode uint16, payload []byte) []byte {
	b := make([]byte, 8+len(payload))
	binary.LittleEndian.PutUint32(b[0:4], obj)
	binary.LittleEndian.PutUint16(b[4:6], opcode)
	binary.LittleEndian.PutUint16(b[6:8], uint16(8+len(payload)))
	copy(b[8:], payload)
	return b
}

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

func newTestParser(t *testing.T) *Parser {
	db := protocol.New()
	if err := db.LoadEmbedded(); err != nil {
		t.Fatal(err)
	}
	return New(db)
}

func concat(bs ...[]byte) []byte {
	var out []byte
	for _, b := range bs {
		out = append(out, b...)
	}
	return out
}

func TestWaylandBasicFlow(t *testing.T) {
	p := newTestParser(t)
	now := time.Now()

	msgs, _ := p.Feed(DirC2S, wlMsg(1, 1, wlU32(2)), 0, now)
	if len(msgs) != 1 || msgs[0].Name != "get_registry" {
		t.Fatalf("get_registry 未识别: %+v", msgs)
	}
	if msgs[0].Iface != "wl_display" || msgs[0].Object != "wl_display@1" {
		t.Fatalf("iface/object 错误: %+v", msgs[0])
	}
	if !strings.Contains(msgs[0].Detail, "registry=wl_registry@2") {
		t.Fatalf("参数未解码: %q", msgs[0].Detail)
	}

	msgs, _ = p.Feed(DirS2C, wlMsg(2, 0, concat(wlU32(1), wlStr("wl_compositor"), wlU32(5))), 0, now)
	if len(msgs) != 1 || msgs[0].Name != "global" {
		t.Fatalf("global 事件未识别: %+v", msgs)
	}
	if !strings.Contains(msgs[0].Detail, `interface="wl_compositor"`) {
		t.Fatalf("global 参数未解码: %q", msgs[0].Detail)
	}

	msgs, _ = p.Feed(DirC2S, wlMsg(2, 0, concat(wlU32(1), wlStr("wl_compositor"), wlU32(5), wlU32(6))), 0, now)
	if len(msgs) != 1 || msgs[0].Name != "bind" {
		t.Fatalf("bind 未识别: %+v", msgs)
	}
	if !strings.Contains(msgs[0].Detail, "id=wl_compositor@6 v5") {
		t.Fatalf("bind 参数未解码: %q", msgs[0].Detail)
	}

	msgs, _ = p.Feed(DirC2S, wlMsg(6, 0, wlU32(7)), 0, now)
	if len(msgs) != 1 || !strings.Contains(msgs[0].Detail, "wl_surface@7") {
		t.Fatalf("create_surface 未正确解码: %+v", msgs)
	}

	msgs, _ = p.Feed(DirC2S, wlMsg(7, 1, concat(wlU32(8), wlU32(0), wlU32(0))), 0, now)
	if len(msgs) != 1 || msgs[0].Name != "attach" {
		t.Fatalf("attach 未识别: %+v", msgs)
	}
	if !strings.Contains(msgs[0].Detail, "x=0") {
		t.Fatalf("attach 参数: %q", msgs[0].Detail)
	}

	msgs, _ = p.Feed(DirS2C, wlMsg(1, 1, wlU32(8)), 0, now)
	if len(msgs) != 1 || msgs[0].Name != "delete_id" {
		t.Fatalf("delete_id 未识别: %+v", msgs)
	}

	msgs, _ = p.Feed(DirC2S, wlMsg(7, 1, concat(wlU32(8), wlU32(0), wlU32(0))), 0, now)
	if len(msgs) != 1 || !strings.Contains(msgs[0].Detail, "buffer=object@8") {
		t.Fatalf("删除后的对象应显示为 object@8: %q", msgs[0].Detail)
	}

	s := p.Summary()
	if !strings.Contains(s["registry_globals"], "wl_compositor") {
		t.Fatalf("registry_globals 缺失: %+v", s)
	}
	if !strings.Contains(s["bound_objects"], "wl_compositor×1") {
		t.Fatalf("bound_objects 缺失: %+v", s)
	}
}

func TestWaylandFdArgs(t *testing.T) {
	p := newTestParser(t)
	now := time.Now()

	p.Feed(DirC2S, wlMsg(1, 1, wlU32(2)), 0, now)
	p.Feed(DirC2S, wlMsg(2, 0, concat(wlU32(1), wlStr("wl_shm"), wlU32(1), wlU32(10))), 0, now)

	msgs, _ := p.Feed(DirC2S, wlMsg(10, 0, concat(wlU32(11), wlU32(4096))), 1, now)
	if len(msgs) != 1 || msgs[0].Name != "create_pool" {
		t.Fatalf("create_pool 未识别: %+v", msgs)
	}
	if !strings.Contains(msgs[0].Detail, "fd=fd#0") || !strings.Contains(msgs[0].Detail, "size=4096") {
		t.Fatalf("fd 参数未解码: %q", msgs[0].Detail)
	}

	msgs, _ = p.Feed(DirC2S, wlMsg(10, 0, concat(wlU32(12), wlU32(8192))), 1, now)
	if !strings.Contains(msgs[0].Detail, "fd=fd#1") {
		t.Fatalf("fd 序号应递增: %q", msgs[0].Detail)
	}
}

func TestWaylandChunkedInput(t *testing.T) {
	p := newTestParser(t)
	now := time.Now()
	full := wlMsg(1, 1, wlU32(2))

	half := len(full) / 2
	msgs, _ := p.Feed(DirC2S, full[:half], 0, now)
	if len(msgs) != 0 {
		t.Fatalf("半条消息不应解码: %+v", msgs)
	}
	msgs, _ = p.Feed(DirC2S, full[half:], 0, now)
	if len(msgs) != 1 || msgs[0].Name != "get_registry" {
		t.Fatalf("分块消息应拼齐后解码: %+v", msgs)
	}
}

func TestWaylandUnknownInterfaceFallback(t *testing.T) {
	p := newTestParser(t)
	now := time.Now()
	msgs, _ := p.Feed(DirS2C, wlMsg(300, 7, wlU32(1)), 0, now)
	if len(msgs) != 1 {
		t.Fatal("未知对象消息仍应记录")
	}
	if msgs[0].Name != "opcode-7" || !strings.Contains(msgs[0].Detail, "raw:") {
		t.Fatalf("fallback 格式错误: %+v", msgs[0])
	}
}
