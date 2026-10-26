package main

import (
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"time"

	"wayland-track/internal/protocol"
	"wayland-track/internal/wayland"
)

func main() {
	rd := os.Getenv("XDG_RUNTIME_DIR")
	sock := os.Getenv("WT_SOCK")
	if sock == "" {
		sock = rd + "/wayland-0"
	}
	c, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: sock, Net: "unix"})
	if err != nil {
		fmt.Println("dial:", err)
		return
	}
	defer c.Close()

	db := protocol.New()
	db.LoadEmbedded()
	p := wayland.New(db)

	hdr := make([]byte, 8)
	binary.LittleEndian.PutUint32(hdr[0:4], 1)
	binary.LittleEndian.PutUint16(hdr[4:6], 1)
	binary.LittleEndian.PutUint16(hdr[6:8], 12)
	body := make([]byte, 4)
	binary.LittleEndian.PutUint32(body, 2)
	c.Write(append(hdr, body...))

	buf := make([]byte, 1<<16)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, _, _, err := c.ReadMsgUnix(buf, nil)
	fmt.Printf("read %d bytes err=%v\n", n, err)
	fmt.Printf("first 64: % x\n", buf[:min64(64, n)])

	msgs, _ := p.Feed(0, hdr[0:0], 0, time.Now())
	msgs, _ = p.Feed(0, append(hdr, body...), 0, time.Now())
	fmt.Printf("C2S msgs: %d\n", len(msgs))
	msgs, _ = p.Feed(1, buf[:n], 0, time.Now())
	fmt.Printf("S2C msgs: %d\n", len(msgs))
	for i, m := range msgs {
		if i < 8 {
			fmt.Printf("  %s %s@%s %s %s\n", m.Dir, m.Iface, m.Object, m.Name, m.Detail)
		}
	}
	s := p.Summary()
	fmt.Println("summary:", s)
}

func min64(a, b int) int {
	if a < b {
		return a
	}
	return b
}
