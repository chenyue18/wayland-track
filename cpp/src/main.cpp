#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "daemon.hpp"
#include "xwayland.hpp"
#include "logger.hpp"
#include "takeover.hpp"

extern volatile sig_atomic_t g_sig_stop;

struct CommonOpts {
    std::string report_dir;
    std::vector<std::string> protocols_dirs = {"/usr/share/wayland", "/usr/share/wayland-protocols"};
    bool no_wayland = false;
    bool no_x11 = false;
    std::string format = "both";
    int max_msgs = 50000;
};

static void usage() {
    printf(
        "wayland-track (C++) — 记录应用使用的 Wayland / X11 协议\n"
        "\n"
        "用法:\n"
        "  wayland-track daemon [flags]     启动守护进程，接管 Wayland/X11 socket 并记录所有应用\n"
        "  wayland-track launch [flags] -- CMD [ARGS...]\n"
        "                                   以受控方式启动单个应用并生成报告\n"
        "  wayland-track xwayland [--display N] -- CMD [ARGS...]\n"
        "                                   经受控 Xwayland 启动 X11 应用，生成 X11/Wayland 关联对照报告\n"
        "  wayland-track restore            恢复被接管的 socket\n"
        "  wayland-track status             查看守护进程与接管状态\n"
        "\n"
        "daemon/launch flags:\n"
        "  --report-dir DIR     报告输出目录 (默认 $XDG_RUNTIME_DIR/wayland-track/reports)\n"
        "  --protocols-dir LIST 额外的协议 XML 目录，逗号分隔\n"
        "  --no-wayland         不监控 Wayland\n"
        "  --no-x11             不监控 X11\n"
        "  --format md|json|both\n"
        "  --max-msgs N         每条连接最多记录的消息数 (默认 50000)\n"
        "\n"
        "daemon flags:\n"
        "  -d                   后台运行 (daemonize)\n"
        "  --log-file FILE      日志文件\n"
        "  --displays LIST      只接管指定 socket，逗号分隔 (如 wayland-0,X0)\n");
}

static std::string runtime_dir() {
    const char* rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) {
        fprintf(stderr, "错误: XDG_RUNTIME_DIR 未设置（需要在图形会话内运行）\n");
        return "";
    }
    return rd;
}

static std::string default_report_dir(const std::string& rd) {
    return rd + "/wayland-track/reports";
}

static DaemonOptions build_options(const std::string& rd, const CommonOpts& o) {
    DaemonOptions opts;
    opts.runtime_dir = rd;
    opts.report_dir = o.report_dir.empty() ? default_report_dir(rd) : o.report_dir;
    opts.protocols_dirs = o.protocols_dirs;
    opts.no_wayland = o.no_wayland;
    opts.no_x11 = o.no_x11;
    opts.format = o.format;
    opts.max_msgs = o.max_msgs;
    return opts;
}

struct ParsedFlags {
    CommonOpts common;
    bool daemonize = false;
    std::string log_file;
    std::vector<std::string> displays;
    std::vector<std::string> positional;
};

static ParsedFlags parse_flags(const std::vector<std::string>& args, bool allow_daemon_flags) {
    ParsedFlags pf;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto value = [&]() -> std::string {
            if (i + 1 < args.size()) return args[++i];
            return "";
        };
        if (a == "--report-dir") {
            pf.common.report_dir = value();
        } else if (a == "--protocols-dir") {
            std::string v = value();
            pf.common.protocols_dirs.clear();
            for (auto& s : split_str(v, ',')) {
                std::string t = trim_str(s);
                if (!t.empty()) pf.common.protocols_dirs.push_back(t);
            }
        } else if (a == "--no-wayland") {
            pf.common.no_wayland = true;
        } else if (a == "--no-x11") {
            pf.common.no_x11 = true;
        } else if (a == "--format") {
            pf.common.format = value();
        } else if (a == "--max-msgs") {
            pf.common.max_msgs = atoi(value().c_str());
        } else if (allow_daemon_flags && (a == "-d" || a == "--daemonize")) {
            pf.daemonize = true;
        } else if (allow_daemon_flags && a == "--log-file") {
            pf.log_file = value();
        } else if (allow_daemon_flags && a == "--displays") {
            std::string v = value();
            for (auto& s : split_str(v, ',')) {
                std::string t = trim_str(s);
                if (!t.empty()) pf.displays.push_back(t);
            }
        } else if (a == "--") {
            for (size_t j = i + 1; j < args.size(); j++) pf.positional.push_back(args[j]);
            break;
        } else if (a == "-h" || a == "--help") {
            usage();
            exit(0);
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "未知参数: %s\n（提示: 后台运行用 daemon -d，各子命令参数见 wayland-track help）\n", a.c_str());
            exit(2);
        } else {
            pf.positional.push_back(a);
        }
    }
    return pf;
}

static int cmd_daemon(const std::vector<std::string>& args) {
    ParsedFlags pf = parse_flags(args, true);
    std::string rd = runtime_dir();
    if (rd.empty()) return 1;

    if (pf.daemonize) {
        std::string log_path = pf.log_file.empty() ? rd + "/wayland-track/daemon.log" : pf.log_file;
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "fork 失败: %s\n", strerror(errno));
            return 1;
        }
        if (pid > 0) {
            printf("已在后台启动 (pid=%d)\n  日志: %s\n  报告: %s\n", pid, log_path.c_str(),
                   pf.common.report_dir.empty() ? default_report_dir(rd).c_str() : pf.common.report_dir.c_str());
            return 0;
        }
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
            close(devnull);
        }
        pf.log_file = log_path;
    }
    if (!pf.log_file.empty()) {
        mkdir_p(to_abs_path(pf.log_file).substr(0, to_abs_path(pf.log_file).rfind('/')), 0755);
        set_log_file(pf.log_file);
    }

    DaemonOptions opts = build_options(rd, pf.common);
    opts.displays = pf.displays;
    Daemon d(opts);
    return d.run();
}

static int cmd_launch(const std::vector<std::string>& args) {
    ParsedFlags pf = parse_flags(args, false);
    if (pf.positional.empty()) {
        fprintf(stderr, "用法: wayland-track launch [flags] -- CMD [ARGS...]\n");
        return 2;
    }
    std::string rd = runtime_dir();
    if (rd.empty()) return 1;
    LaunchOptions lo;
    lo.opts = build_options(rd, pf.common);
    lo.command = pf.positional;
    return run_launch(lo);
}

static int daemon_pid(const std::string& rd) {
    std::string s;
    if (!read_file_str(rd + "/wayland-track/daemon.pid", s)) return 0;
    return atoi(trim_str(s).c_str());
}

static bool pid_alive(int pid) {
    if (pid <= 0) return false;
    return kill(pid, 0) == 0;
}

static int cmd_xwayland(const std::vector<std::string>& args) {
    ParsedFlags pf = parse_flags(args, false);
    int display = -1;
    std::vector<std::string> rest;
    for (size_t i = 0; i < pf.positional.size(); i++) {
        if (pf.positional[i] == "--display" && i + 1 < pf.positional.size()) {
            display = atoi(pf.positional[i + 1].c_str());
            i++;
            continue;
        }
        rest.push_back(pf.positional[i]);
    }
    if (rest.empty()) {
        fprintf(stderr, "用法: wayland-track xwayland [--display N] -- CMD [ARGS...]\n");
        return 2;
    }
    std::string rd = runtime_dir();
    if (rd.empty()) return 1;
    XwaylandOptions xo;
    xo.base = build_options(rd, pf.common);
    xo.command = rest;
    xo.display = display;
    return run_xwayland(xo);
}

static int cmd_restore(const std::vector<std::string>& args) {
    (void)args;
    std::string rd = runtime_dir();
    if (rd.empty()) return 1;
    int pid = daemon_pid(rd);
    if (pid > 0 && pid_alive(pid)) {
        printf("守护进程 pid=%d 正在运行，发送 SIGTERM...", pid);
        fflush(stdout);
        kill(pid, SIGTERM);
        for (int i = 0; i < 30; i++) {
            if (!pid_alive(pid)) break;
            usleep(100 * 1000);
        }
        printf(" 已停止\n");
    }
    for (auto& m : takeover::restore(rd)) printf("%s\n", m.c_str());
    return 0;
}

static int cmd_status(const std::vector<std::string>& args) {
    (void)args;
    std::string rd = runtime_dir();
    if (rd.empty()) return 1;
    int pid = daemon_pid(rd);
    if (pid > 0 && pid_alive(pid))
        printf("守护进程: 运行中 (pid=%d)\n", pid);
    else
        printf("守护进程: 未运行\n");
    TrackState st = takeover::load_state(rd);
    if (st.targets.empty() && st.pid == 0 && st.report_dir.empty()) {
        printf("接管状态: 无\n");
        return 0;
    }
    printf("接管状态: %zu 个 socket\n", st.targets.size());
    for (auto& t : st.targets) printf("  [%s] %s -> %s\n", t.proto.c_str(), t.proxy.c_str(), t.real.c_str());
    printf("报告目录: %s\n", st.report_dir.c_str());
    return 0;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    install_signal_handlers();
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    if (args.empty()) {
        usage();
        return 2;
    }
    std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "daemon") return cmd_daemon(rest);
    if (cmd == "launch") return cmd_launch(rest);
    if (cmd == "xwayland") return cmd_xwayland(rest);
    if (cmd == "restore") return cmd_restore(rest);
    if (cmd == "status") return cmd_status(rest);
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        usage();
        return 0;
    }
    fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
    usage();
    return 2;
}
