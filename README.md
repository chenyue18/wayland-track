# wayland-track

> 两个实现：**Go**（本目录，`go build`）与 **C++17**（`cpp/`，`make`），功能与行为一致，详见 [cpp/README.md](cpp/README.md)。

记录图形应用使用了哪些 **Wayland / X11** 协议接口：常驻后台，应用退出后自动为它生成一份协议使用报告（Markdown + JSON），包含**哪个应用、每条连接、每个协议接口、每条消息及其参数**。

## 工作原理

`wayland-track` 采用 **透明 socket 代理**（xtrace 风格）方案，无需 root、无需 LD_PRELOAD、不修改应用：

```
应用 ──> /run/user/1000/wayland-0 (代理监听) ──> wayland-0.wt-real (真实 compositor)
应用 ──> /tmp/.X11-unix/X0       (代理监听) ──> X0.wt-real       (真实 X server)
```

1. **接管**：守护进程启动时把真实 socket 重命名（`wayland-0` → `wayland-0.wt-real`），在原路径上创建代理监听。对应用完全透明，桌面会话不受影响。
2. **识别应用**：通过 `SO_PEERCRED` 获得连接方 pid/uid，读取 `/proc/<pid>` 得到进程名、可执行文件、命令行。
3. **解析协议**（边转发边解析，双向）：
   - **Wayland**：按官方 wire 格式解码（object id / opcode / 参数），接口定义内嵌了 wayland 核心协议 + [wayland-protocols](https://wayland.app/protocols/) 全部 59 份协议（与上游同步：`cpp/tools/gen_embedded_proto.py` / 重新构建即可），并可在运行时通过 `--protocols-dir` 加载更多定义；通过 `wl_registry` 动态跟踪所有对象，因此即使协议 XML 缺失也能记录接口名。支持 fd 传递参数（`wl_shm`、`wl_keyboard.keymap` 等）。
   - **X11**：解析连接握手（字节序、认证方式）、核心协议全部 120 种请求（ opcode 表由 `tools/xproto.xml` 生成）、通过 `QueryExtension` 动态发现扩展（BIG-REQUESTS、XKEYBOARD、RANDR 等）、区分事件/错误/回复，并为常用请求解码参数（窗口/像素图/GC id、atom 名、属性类型等）。atom 名称通过 `InternAtom` 动态学习。
4. **应用退出后**（pid 消失或连接全部关闭），自动写出报告。

## 构建

需要 Go 1.21+：

```sh
go build -o wayland-track ./cmd/wayland-track
```

## 使用

### 1) 常驻监控整个会话（推荐在会话启动时运行）

```sh
wayland-track daemon            # 前台运行
wayland-track daemon -d         # 后台运行（daemonize）
```

之后启动的每个图形应用（Wayland 或 X11）都会被记录，应用退出后报告写入
`$XDG_RUNTIME_DIR/wayland-track/reports/`（可用 `--report-dir` 修改）。

```sh
wayland-track status            # 查看守护进程与接管状态
wayland-track restore           # 恢复 socket 并停止守护进程（异常退出后也可用，会自动扫描恢复）
```

常用参数：

| 参数 | 说明 |
|---|---|
| `--displays wayland-0,X0` | 只接管指定 socket |
| `--no-x11` / `--no-wayland` | 关闭某一协议的监控 |
| `--format md\|json\|both` | 报告格式（默认 both） |
| `--max-msgs N` | 每连接最多记录的消息条数（默认 50000，统计数字不受影响） |
| `--protocols-dir DIR1,DIR2` | 额外的协议 XML 目录（默认包含 `/usr/share/wayland-protocols`） |

### 2) 只监控某个应用（launch 模式）

```sh
wayland-track launch -- gedit
wayland-track launch --report-dir ./out -- xdpyinfo
```

通过环境变量（`WAYLAND_DISPLAY` / `DISPLAY`）把该应用导向一次性代理 socket，不改动系统会话，退出即出报告。

## 报告示例

```markdown
# 应用图形协议使用记录
## 应用信息
| 应用 | gnome-calculator (pid 56477) |
| 可执行文件 | `/usr/bin/gnome-calculator` |
## 概览
| Wayland | 3 | 2954 |
## 连接 #1 — Wayland（/run/user/1000/wayland-0）
### 使用的 Wayland 接口
| wl_surface | 589 | 2 |
| xdg_toplevel | 8 | 5 |
### 消息日志
| # | 时刻 | 方向 | 对象 | 消息 | 参数 | 字节 |
| 1 | 12:01:05.339 | C→S | wl_display@1 | get_registry | registry=wl_registry@2 | 12 |
| 2 | 12:01:05.339 | C→S | wl_compositor@4 | create_surface | surface=wl_surface@42 | 12 |
| 3 | 12:01:05.341 | C→S | xdg_wm_base@41 | get_xdg_surface | id=xdg_surface@43, surface=wl_surface@42 | 16 |
```

## 目录结构

```
cmd/wayland-track/      主程序
cmd/probe/              调试小工具：直接连接 compositor 打印解码后的流量
internal/protocol/      Wayland 协议 XML 数据库（go:embed 内嵌核心协议 + wayland-protocols 全部 59 份）
internal/wayland/       Wayland wire 协议解析器
internal/x11/           X11 流解析器（opcode 表由 tools/xproto.xml 生成）
internal/relay/         透明转发（保留 SCM_RIGHTS fd 传递）
internal/takeover/      socket 接管 / 恢复 / 状态文件
internal/report/        按应用聚合 + Markdown/JSON 报告
internal/daemon/        守护进程编排（接管、accept、退出清理）
tools/xproto.xml        X11 协议定义（用于生成 internal/x11/opcodes_req_gen.go）
```

## 已知限制

- **X11 abstract socket**：Xwayland 同时监听文件系统 socket 和 abstract socket（`@/tmp/.X11-unix/X0`），libxcb 客户端（含 Electron/VS Code 等）通常优先使用 abstract。daemon 会尝试把 abstract 也一并接管（成功则完整覆盖）；若 X server 已占用（EADDRINUSE）会在日志中警告，此时监控 X11 应用请用 `launch` 模式（代理同时占用文件系统+abstract，已实测完整捕获 snap 版 VS Code 的全部 5 个进程）。Wayland 协议没有这个问题。
- **报告缺失排查**：① `status` 确认 daemon 活着且列出接管项；② 日志中无 `连接 #N` = 应用没经过代理（应用先于 daemon 启动 / X11 走了 abstract / snap 自带代理层），改用 `launch`；③ 有连接行但无报告 = daemon 被强杀（SIGKILL 无法收尾），`restore` 后重开；④ daemon 异常死亡可能留下死代理 socket，新版启动时会自动清理。
- 只有接管/代理之后**新建立**的连接会被记录；守护进程启动前已连接的进程（如 Xwayland、桌面 shell 自身）不在记录范围。
- 静态扫描 `$XDG_RUNTIME_DIR` 和 `/tmp/.X11-unix`，运行中途新出现的 socket（如第二个 display）不会自动接管。
- Wayland 接口定义不全时（未安装对应协议 XML），消息仍会记录（接口名来自 `wl_registry`），参数以原始 32 位字显示；X11 扩展的 minor opcode 暂不翻译为请求名。
- X11 大请求（BIG-REQUESTS）、GE 泛型事件已支持；扩展专属事件统一按 32 字节处理。

## 许可

代码为项目自有实现。内嵌协议 XML 来自 freedesktop.org（wayland / wayland-protocols，MIT 许可；xproto 参考自 xcb-proto，MIT 许可）。
