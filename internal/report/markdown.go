package report

import (
	"encoding/json"
	"fmt"
	"os"
)

func writeJSON(path string, rep *Report) error {
	b, err := json.MarshalIndent(rep, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	return os.WriteFile(path, b, 0o644)
}

func writeMarkdown(path string, rep *Report) error {
	var b []byte
	b = appendHeader(b, rep)
	b = appendConnSections(b, rep)
	return os.WriteFile(path, b, 0o644)
}

func appendHeader(b []byte, rep *Report) []byte {
	app := rep.App
	protoCount := map[string]int{}
	msgTotal := 0
	for i := range rep.Connections {
		c := &rep.Connections[i]
		protoCount[c.Proto]++
		msgTotal += c.MsgCount
	}
	b = str(b, "# 应用图形协议使用记录\n\n")
	b = appendf(b, "> 由 wayland-track 自动生成。应用退出后，记录其在 Wayland / X11 连接中使用的协议接口与消息参数。\n\n")
	b = str(b, "## 应用信息\n\n")
	b = str(b, "| 项 | 值 |\n|---|---|\n")
	b = appendf(b, "| 应用 | %s (pid %d) |\n", md(app.Comm), app.Pid)
	if app.Exe != "" {
		b = appendf(b, "| 可执行文件 | `%s` |\n", md(app.Exe))
	}
	if app.Cmdline != "" {
		b = appendf(b, "| 命令行 | `%s` |\n", md(app.Cmdline))
	}
	b = appendf(b, "| 用户 | uid=%d gid=%d |\n", app.Uid, app.Gid)
	b = appendf(b, "| 首次连接 | %s |\n", fmtTime(rep.FirstSeen))
	b = appendf(b, "| 报告生成 | %s |\n", fmtTime(rep.GeneratedAt))
	b = str(b, "\n## 概览\n\n")
	b = str(b, "| 协议 | 连接数 | 消息总数 |\n|---|---|---|\n")
	order := []string{"Wayland", "X11"}
	seen := map[string]bool{}
	for _, p := range order {
		if n, ok := protoCount[p]; ok {
			b = appendf(b, "| %s | %d | %d |\n", p, n, sumProtoMsgs(rep, p))
			seen[p] = true
		}
	}
	for p := range protoCount {
		if !seen[p] {
			b = appendf(b, "| %s | %d | %d |\n", md(p), protoCount[p], sumProtoMsgs(rep, p))
		}
	}
	_ = msgTotal
	b = str(b, "\n")
	return b
}

func sumProtoMsgs(rep *Report, proto string) int {
	n := 0
	for i := range rep.Connections {
		if rep.Connections[i].Proto == proto {
			n += rep.Connections[i].MsgCount
		}
	}
	return n
}

func appendConnSections(b []byte, rep *Report) []byte {
	for i := range rep.Connections {
		c := &rep.Connections[i]
		b = appendf(b, "---\n\n## 连接 #%d — %s（%s）\n\n", c.ID, c.Proto, md(c.Socket))
		b = appendf(b, "- 开始: %s\n", fmtTime(c.Start))
		if !c.End.IsZero() {
			b = appendf(b, "- 结束: %s\n", fmtTime(c.End))
		} else {
			b = appendf(b, "- 结束: (连接仍活跃或未正常关闭，报告生成时截止)\n")
		}
		b = appendf(b, "- 消息总数: %d\n", c.MsgCount)
		if c.Truncated {
			b = appendf(b, "- 注意: 消息日志超出上限，仅记录前 %d 条（统计数字仍为完整值）\n", c.Logged)
		}
		b = str(b, "\n")

		if len(c.Meta) > 0 {
			b = str(b, "### 会话信息\n\n")
			keys := make([]string, 0, len(c.Meta))
			for k := range c.Meta {
				keys = append(keys, k)
			}
			sortStrings(keys)
			for _, k := range keys {
				b = appendf(b, "- %s: %s\n", k, md(c.Meta[k]))
			}
			b = str(b, "\n")
		}

		if len(c.Stats) > 0 {
			if c.Proto == "Wayland" {
				b = str(b, "### 使用的 Wayland 接口\n\n")
				b = str(b, "| 接口 | 客户端请求 | 服务端事件 |\n|---|---|---|\n")
			} else {
				b = str(b, "### X11 协议使用统计\n\n")
				b = str(b, "| 协议/扩展 | 客户端请求 | 服务端回复/事件 |\n|---|---|---|\n")
			}
			for _, name := range ifacesSorted(c.Stats) {
				st := c.Stats[name]
				b = appendf(b, "| %s | %d | %d |\n", md(name), st.Sent, st.Recv)
			}
			b = str(b, "\n")
		}

		b = str(b, "### 消息日志\n\n")
		if len(c.Msgs) == 0 {
			b = str(b, "(无消息记录)\n\n")
			continue
		}
		b = str(b, "| # | 时刻 | 方向 | 对象 | 消息 | 参数 | 字节 |\n")
		b = str(b, "|---|---|---|---|---|---|---|\n")
		for j := range c.Msgs {
			m := &c.Msgs[j]
			detail := m.Detail
			if len(detail) > 300 {
				detail = detail[:300] + "…"
			}
			b = appendf(b, "| %d | %s | %s | %s | %s | %s | %d |\n",
				j+1, m.Time.Format("15:04:05.000"), m.Dir, md(m.Object), md(m.Name), md(detail), m.Bytes)
		}
		b = str(b, "\n")
	}
	return b
}


func str(b []byte, s string) []byte {
	return append(b, s...)
}

func appendf(b []byte, f string, a ...any) []byte {
	return append(b, fmt.Sprintf(f, a...)...)
}

func md(s string) string {
	out := make([]byte, 0, len(s))
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '|':
			out = append(out, '\\', '|')
		case '\n', '\r':
			out = append(out, ' ')
		default:
			out = append(out, c)
		}
	}
	return string(out)
}

func sortStrings(s []string) {
	for i := 1; i < len(s); i++ {
		for j := i; j > 0 && s[j] < s[j-1]; j-- {
			s[j], s[j-1] = s[j-1], s[j]
		}
	}
}
