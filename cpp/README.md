# wayland-track (C++ 实现)

Go 版本的 C++17 移植，功能与行为一致：常驻后台透明代理 Wayland / X11 socket，
识别应用、解析协议（含参数），应用退出后自动生成 Markdown + JSON 报告。

## 构建

仅需 g++（无需第三方库，协议 XML 由脚本嵌入）：

```sh
make            # 生成 build/wayland-track（静态链接 libstdc++/libgcc）
make test       # 构建并运行测试（90 项检查：XML/协议解析/集成）
make gen        # 重新生成内嵌协议表（需要 python3）
```

## 用法

与 Go 版完全相同的子命令：

```sh
build/wayland-track daemon -d       # 后台常驻，接管 wayland-0 + X0/X1
build/wayland-track launch -- gedit # 只监控单个应用（env 重定向到一次性代理）
build/wayland-track status
build/wayland-track restore
```

参数（`--report-dir`、`--displays`、`--no-x11`、`--format`、`--max-msgs`、
`--log-file` 等）与 Go 版一致，见仓库根目录 README。

## 结构

```
src/
  main.cpp          CLI（daemon/launch/restore/status）
  daemon.cpp        守护进程编排、launch 模式、acceptor/pump 线程
  takeover.cpp      socket 接管/恢复/状态文件/abstract+fs 监听
  relay.cpp         recvmsg/sendmsg 透传（SCM_RIGHTS fd 传递、半写处理）
  wayland.cpp       Wayland wire 解析（registry 对象跟踪、fd 参数）
  x11.cpp           X11 流解析（握手/请求/扩展/事件，opcode 表生成）
  report.cpp        按应用聚合 + Markdown/JSON 报告
  proto_db.cpp      协议定义数据库（运行时可加载额外 XML）
  xml_min.cpp       极简 XML 解析器（仅用于运行时加载）
  gen/              生成代码：wayland 核心 + wayland-protocols 全部 59 份协议 XML、X11 opcode 表
tools/
  gen_embedded_proto.py   proto/*.xml → src/gen/embedded_proto.cpp
  gen_x11.py              xproto.xml → src/gen/x11_tables.cpp
proto/              内嵌的协议 XML 源（重新生成用）
```

## 与 Go 版的实现差异

- 并发：每条连接 2 个 detached pump 线程 + 每监听器 1 个 acceptor 线程
  （非阻塞 poll，250ms 轮询停止标志），共享状态用 `std::mutex` 保护
- 协议 XML：wayland 核心 + wayland-protocols 全量（59 份）编译期内嵌（raw string literal），同时保留运行时 `--protocols-dir` 加载
- 守护进程化：fork + setsid（Go 版为 re-exec）
- 状态文件同为 JSON（手工读写），但两版不宜混用接管状态

## 实测（GNOME Wayland 会话）

- `wayland-info`：149 条消息全解码；`gnome-calculator`：3007 条消息、34 种接口绑定
- `xdpyinfo` 经 launch 代理：握手 + BIG-REQUESTS/XKEYBOARD 扩展发现正常
- daemon → restore 后会话无感知
