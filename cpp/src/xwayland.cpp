#include "xwayland.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "logger.hpp"
#include "procinfo.hpp"
#include "relay.hpp"
#include "report.hpp"
#include "util.hpp"
#include "wayland.hpp"
#include "x11.hpp"

extern volatile sig_atomic_t g_sig_stop;

namespace {

struct XwState {
    std::shared_ptr<const ProtoDB> db;
    std::shared_ptr<Tracker> tracker;
    std::atomic<bool>* stop;
};

int connect_unix_path(const std::string& path) {
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0) return fd;
    close(fd);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = '\0';
    strncpy(sa.sun_path + 1, path.c_str(), sizeof(sa.sun_path) - 2);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    socklen_t len = socklen_t(offsetof(struct sockaddr_un, sun_path) + 1 + path.size());
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void xw_accept_loop(int listen_fd, const Target& t, XwState st) {
    while (!st.stop->load()) {
        struct pollfd pfd{listen_fd, POLLIN, 0};
        if (poll(&pfd, 1, 250) <= 0) continue;
        if (pfd.revents & POLLNVAL) break;
        while (true) {
            int cfd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                break;
            }
            int pid = 0, uid = 0, gid = 0;
            if (!takeover::peer_cred(cfd, pid, uid, gid)) {
                close(cfd);
                continue;
            }
            ProcInfo app = procinfo_read(pid);
            AppInfo ai;
            ai.pid = pid;
            ai.uid = uid;
            ai.gid = gid;
            ai.comm = app.comm;
            ai.exe = app.exe;
            ai.cmdline = app.cmdline;
            int real_fd = connect_unix_path(t.real);
            if (real_fd < 0) {
                logf("连接真实 socket %s 失败: %s", t.real.c_str(), strerror(errno));
                close(cfd);
                continue;
            }
            std::shared_ptr<Parser> parser;
            if (t.proto == "Wayland")
                parser = std::make_shared<WaylandParser>(st.db);
            else
                parser = std::make_shared<X11Parser>();
            uint64_t conn_id = st.tracker->open_conn(pid, t.proto, t.proxy, ai, parser);
            auto ctx = std::make_shared<PumpCtx>();
            ctx->client_fd = cfd;
            ctx->real_fd = real_fd;
            ctx->parser = parser;
            ctx->tracker = st.tracker;
            ctx->conn_id = conn_id;
            ctx->remaining = std::make_shared<std::atomic<int>>(2);
            std::thread(relay::pump, cfd, real_fd, relay::DIR_C2S, ctx).detach();
            std::thread(relay::pump, real_fd, cfd, relay::DIR_S2C, ctx).detach();
        }
    }
}

std::vector<std::string> base_env() {
    std::vector<std::string> out;
    for (char** e = environ; *e; e++) out.push_back(*e);
    return out;
}

void apply_env(std::vector<std::string>& env, const std::string& key, const std::string& value) {
    std::string prefix = key + "=";
    for (auto& kv : env)
        if (kv.rfind(prefix, 0) == 0) {
            kv = prefix + value;
            return;
        }
    env.push_back(prefix + value);
}

pid_t spawn(const std::vector<std::string>& cmd, std::vector<std::string>& env) {
    std::vector<char*> argv;
    for (auto& c : cmd) argv.push_back(const_cast<char*>(c.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        execvpe(cmd[0].c_str(), argv.data(), envp.data());
        fprintf(stderr, "exec %s 失败: %s\n", cmd[0].c_str(), strerror(errno));
        _exit(127);
    }
    return pid;
}

int wait_child(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
        if (g_sig_stop) return -1;
    }
    return status;
}

}  // namespace

int run_xwayland(const XwaylandOptions& opts) {
    if (opts.command.empty()) {
        logf("xwayland 需要指定要启动的应用，例如: wayland-track xwayland -- xmessage hello");
        return 1;
    }
    const DaemonOptions& base = opts.base;

    auto db = std::make_shared<ProtoDB>();
    db->load_embedded();
    db->load_dirs(base.protocols_dirs);

    auto tracker_w = std::make_shared<Tracker>(base.report_dir, base.format, base.max_msgs);
    auto tracker_x = std::make_shared<Tracker>(base.report_dir, base.format, base.max_msgs);
    tracker_w->set_collect_only(true);
    tracker_x->set_collect_only(true);

    const char* rt = base.runtime_dir.c_str();
    std::string real_wayland = getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "wayland-0";
    if (!real_wayland.empty() && real_wayland[0] != '/') real_wayland = std::string(rt) + "/" + real_wayland;
    if (!is_socket_path(real_wayland)) {
        logf("真实 Wayland socket 不存在: %s", real_wayland.c_str());
        return 1;
    }

    std::string proxy_dir = fmt("%s/wayland-track-xwl-%d", rt, getpid());
    mkdir_p(proxy_dir, 0700);
    std::string proxy_sock = proxy_dir + "/wayland-0";
    int lw = takeover::listen_fs(proxy_sock);
    if (lw < 0) {
        logf("创建 Wayland 代理监听失败");
        return 1;
    }

    auto display_free = [](int n) {
        std::string p = fmt("/tmp/.X11-unix/X%d", n);
        if (!is_socket_path(p)) return true;
        int probe = connect_unix_path(p);
        if (probe >= 0) {
            close(probe);
            return false;
        }
        unlink(p.c_str());
        return true;
    };
    int n = opts.display;
    if (n < 0) {
        for (n = 90; n < 100; n++) {
            if (display_free(n)) break;
        }
        if (n >= 100) {
            logf("没有可用的显示号");
            close(lw);
            return 1;
        }
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> acceptors;
    std::vector<int> listen_fds;
    Target wl_target{"Wayland", proxy_sock, real_wayland, fmt(":%d", n), false};

    auto spawn_acceptor = [&](int fd, const Target& t, std::shared_ptr<Tracker> tr) {
        listen_fds.push_back(fd);
        XwState st{db, tr, &stop};
        acceptors.emplace_back([fd, t, st]() { xw_accept_loop(fd, t, st); });
    };
    spawn_acceptor(lw, wl_target, tracker_w);

    auto env_xwayland = base_env();
    apply_env(env_xwayland, "WAYLAND_DISPLAY", proxy_sock);
    pid_t xwl = spawn({"Xwayland", fmt(":%d", n), "-nolisten", "local"}, env_xwayland);
    if (xwl < 0) {
        logf("启动 Xwayland 失败");
        close(lw);
        return 1;
    }

    std::string x_sock = fmt("/tmp/.X11-unix/X%d", n);
    bool ready = false;
    for (int i = 0; i < 100; i++) {
        if (is_socket_path(x_sock)) {
            ready = true;
            break;
        }
        usleep(50 * 1000);
    }
    if (!ready) {
        logf("Xwayland 未在预期时间创建 %s", x_sock.c_str());
        kill(xwl, SIGTERM);
        close(lw);
        return 1;
    }
    struct stat st{};
    mode_t mode = 0777;
    if (stat(x_sock.c_str(), &st) == 0) mode = st.st_mode & 0777;
    std::string x_real = x_sock + ".wt-real";
    if (rename(x_sock.c_str(), x_real.c_str()) != 0) {
        logf("接管 %s 失败: %s", x_sock.c_str(), strerror(errno));
        kill(xwl, SIGTERM);
        close(lw);
        return 1;
    }
    int lx = takeover::listen_fs_mode(x_sock, int(mode));
    int lxa = takeover::listen_abstract(x_sock);
    if (lx < 0) {
        logf("在 %s 上创建 X11 代理监听失败: %s", x_sock.c_str(), strerror(errno));
        rename(x_real.c_str(), x_sock.c_str());
        kill(xwl, SIGTERM);
        close(lw);
        return 1;
    }
    Target x_target{"X11", x_sock, x_real, fmt("X%d", n), false};
    spawn_acceptor(lx, x_target, tracker_x);
    if (lxa >= 0) spawn_acceptor(lxa, x_target, tracker_x);
    logf("受控 Xwayland :%d (pid=%d)，X11 代理 %s，Wayland 代理 %s", n, xwl, x_sock.c_str(), proxy_sock.c_str());

    auto env_app = base_env();
    apply_env(env_app, "DISPLAY", fmt(":%d", n));
    apply_env(env_app, "WAYLAND_DISPLAY", proxy_sock);
    pid_t app = spawn(opts.command, env_app);
    if (app < 0) {
        logf("启动应用失败");
        kill(xwl, SIGTERM);
        stop = true;
        for (auto& th : acceptors) th.join();
        rename(x_real.c_str(), x_sock.c_str());
        close(lw);
        return 1;
    }
    char self_exe[4096] = {0};
    readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
    ProcInfo app_info;
    for (int i = 0; i < 50; i++) {
        app_info = procinfo_read(app);
        if (app_info.exe != self_exe) break;
        usleep(20 * 1000);
    }
    logf("应用已启动 pid=%d (%s)，退出后将生成关联对照报告", app, app_info.comm.c_str());

    int status = wait_child(app);

    kill(xwl, SIGTERM);
    int xs = wait_child(xwl);

    stop = true;
    for (int fd : listen_fds) ::shutdown(fd, SHUT_RDWR);
    for (auto& th : acceptors) th.join();

    if (path_exists(x_real)) {
        rename(x_real.c_str(), x_sock.c_str());
    } else {
        int probe = connect_unix_path(x_sock);
        if (probe < 0) unlink(x_sock.c_str());
        else close(probe);
    }
    unlink(proxy_sock.c_str());
    rmdir(proxy_dir.c_str());

    CorrelatedInput in;
    in.app.pid = app_info.pid;
    in.app.comm = app_info.comm;
    in.app.exe = app_info.exe;
    in.app.cmdline = app_info.cmdline;
    in.display = fmt(":%d", n);
    for (auto& r : tracker_x->finalize_collect())
        for (auto& c : r.connections) in.x11.push_back(std::move(c));
    for (auto& r : tracker_w->finalize_collect())
        for (auto& c : r.connections) in.wayland.push_back(std::move(c));

    std::string base_name = fmt("xwayland_%s_pid%d_%s", fmt_stamp(Clock::now()).c_str(), app_info.pid,
                                sanitize_name(app_info.comm).c_str());
    std::string md = base.report_dir + "/" + base_name + ".md";
    std::string js = base.report_dir + "/" + base_name + ".json";
    write_correlated_markdown(md, in);
    write_correlated_json(js, in);
    logf("关联对照报告已写入: %s + %s", md.c_str(), js.c_str());

    if (WIFEXITED(status))
        logf("应用退出 (%d)", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        logf("应用被信号终止 (%d)", WTERMSIG(status));
    (void)xs;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
