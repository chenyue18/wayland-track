package x11

import (
	"encoding/binary"
	"strings"
	"testing"
	"time"
)

func u16(v uint16) []byte {
	b := make([]byte, 2)
	binary.LittleEndian.PutUint16(b, v)
	return b
}

func u32(v uint32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, v)
	return b
}

func concat(bs ...[]byte) []byte {
	var out []byte
	for _, b := range bs {
		out = append(out, b...)
	}
	return out
}

func req(op byte, minor byte, payload []byte) []byte {
	b := make([]byte, 4+len(payload))
	b[0] = op
	b[1] = minor
	binary.LittleEndian.PutUint16(b[2:4], uint16((4+len(payload))/4))
	copy(b[4:], payload)
	return b
}

func TestX11Flow(t *testing.T) {
	p := New()
	now := time.Now()

	setup := append([]byte{'l', 0}, concat(u16(0), u16(0), u16(0), u16(0), u16(0))...)
	msgs, _ := p.Feed(DirC2S, setup, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "Setup(握手)" {
		t.Fatalf("客户端握手未识别: %+v", msgs)
	}

	vendor := "Test\x00\x00\x00\x00"
	srep := append([]byte{1, 0}, u16(11)...)
	srep = append(srep, u16(0)...)
	srep = append(srep, u16(10)...)
	srep = append(srep, u32(12345)...)
	srep = append(srep, u32(0x00400000)...)
	srep = append(srep, u32(0x001fffff)...)
	srep = append(srep, u32(256)...)
	srep = append(srep, u16(5)...)
	srep = append(srep, u16(65535)...)
	srep = append(srep, []byte{0, 0, 0, 0, 0, 0, 0, 0}...)
	srep = append(srep, []byte{0, 0, 0, 0}...)
	srep = append(srep, vendor...)
	msgs, _ = p.Feed(DirS2C, srep, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "Setup-Success" {
		t.Fatalf("服务器握手未识别: %+v", msgs)
	}
	if !strings.Contains(msgs[0].Detail, "vendor=\"Test\"") {
		t.Fatalf("vendor 未解析: %q", msgs[0].Detail)
	}

	msgs, _ = p.Feed(DirC2S, req(1, 24, concat(u32(0x1c00001), u32(0x1c00000), u32(0))), 0, now)
	if len(msgs) != 1 || msgs[0].Name != "CreateWindow" {
		t.Fatalf("CreateWindow 未识别: %+v", msgs)
	}
	if !strings.Contains(msgs[0].Detail, "win=0x1c00001") || !strings.Contains(msgs[0].Detail, "depth=24") {
		t.Fatalf("CreateWindow 参数: %q", msgs[0].Detail)
	}

	intern := req(16, 1, concat(u16(5), []byte("HELLO\x00")))
	msgs, _ = p.Feed(DirC2S, intern, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "InternAtom" {
		t.Fatalf("InternAtom 未识别: %+v", msgs)
	}

	reply := append([]byte{1, 0}, u16(2)...)
	reply = concat(reply, u32(0), u32(42), make([]byte, 20))
	msgs, _ = p.Feed(DirS2C, reply, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "InternAtom-Reply" {
		t.Fatalf("InternAtom 回复未关联: %+v", msgs)
	}

	qext := req(98, 0, concat(u16(5), u16(0), []byte("RANDR\x00\x00\x00")))
	msgs, _ = p.Feed(DirC2S, qext, 0, now)
	if len(msgs) != 1 || !strings.Contains(msgs[0].Detail, "name=\"RANDR\"") {
		t.Fatalf("QueryExtension 未解码: %+v", msgs)
	}

	qreply := append([]byte{1, 0}, u16(3)...)
	qreply = append(qreply, u32(0)...)
	qreply = concat(qreply, []byte{1, 140, 89, 90}, make([]byte, 20))
	msgs, _ = p.Feed(DirS2C, qreply, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "QueryExtension-Reply" {
		t.Fatalf("QueryExtension 回复未关联: %+v", msgs)
	}

	msgs, _ = p.Feed(DirC2S, req(140, 5, u32(0x1c00001)), 0, now)
	if len(msgs) != 1 || msgs[0].Iface != "RANDR" {
		t.Fatalf("扩展请求未映射: %+v", msgs)
	}

	expose := append([]byte{12, 0}, u16(4)...)
	expose = concat(expose, u32(0x1c00001), make([]byte, 24))
	msgs, _ = p.Feed(DirS2C, expose, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "Expose" {
		t.Fatalf("Expose 事件未识别: %+v", msgs)
	}

	ge := append([]byte{35, 140}, u16(5)...)
	ge = concat(ge, u32(0), u16(1), u16(0), make([]byte, 20))
	msgs, _ = p.Feed(DirS2C, ge, 0, now)
	if len(msgs) != 1 || msgs[0].Iface != "RANDR" {
		t.Fatalf("GE 事件未映射: %+v", msgs)
	}

	errPkt := append([]byte{0, 3}, u16(1)...)
	errPkt = concat(errPkt, make([]byte, 28))
	msgs, _ = p.Feed(DirS2C, errPkt, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "Error:Window" {
		t.Fatalf("错误未识别: %+v", msgs)
	}

	s := p.Summary()
	if !strings.Contains(s["x11_extensions"], "RANDR(op=140") {
		t.Fatalf("扩展列表缺失: %+v", s)
	}
	if !strings.Contains(s["server_vendor"], "Test") {
		t.Fatalf("vendor 摘要缺失: %+v", s)
	}
}

func TestX11Chunked(t *testing.T) {
	p := New()
	now := time.Now()
	p.Feed(DirC2S, []byte{'l', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, now)

	full := req(8, 0, u32(0x1234))
	msgs, _ := p.Feed(DirC2S, full[:5], 0, now)
	if len(msgs) != 0 {
		t.Fatal("半条请求不应解码")
	}
	msgs, _ = p.Feed(DirC2S, full[5:], 0, now)
	if len(msgs) != 1 || msgs[0].Name != "MapWindow" {
		t.Fatalf("分块请求应解码: %+v", msgs)
	}
}

func TestX11BigRequest(t *testing.T) {
	p := New()
	now := time.Now()
	p.Feed(DirC2S, []byte{'l', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, now)

	body := make([]byte, 4+400)
	total := 8 + len(body)
	hdr := []byte{254, 72, 0, 0}
	ext := make([]byte, 4)
	binary.LittleEndian.PutUint32(ext, uint32(total/4))
	big := append(append(hdr, ext...), body...)
	msgs, _ := p.Feed(DirC2S, big, 0, now)
	if len(msgs) != 1 || msgs[0].Name != "PutImage" || msgs[0].Bytes != total {
		t.Fatalf("Big-Request 未处理: %+v", msgs)
	}
}
