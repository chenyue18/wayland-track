#include "daemon.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "logger.hpp"
#include "procinfo.hpp"
#include "relay.hpp"
#include "wayland.hpp"
#include "x11.hpp"

volatile sig_atomic_t g_sig_stop = 0;

static void on_shutdown_signal(int) {
    g_sig_stop = 1;
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
}

Daemon::Daemon(const DaemonOptions& opts) : opts_(opts) {
    if (opts_.max_msgs <= 0) opts_.max_msgs = 50000;
    db_ = std::make_shared<ProtoDB>();
    db_->load_embedded();
    int extra = db_->load_dirs(opts_.protocols_dirs);
    logf("协议定义: %zu 个接口来自内嵌 XML，额外加载 %d 个系统 XML", db_->iface_count(), extra);
    tracker_ = std::make_shared<Tracker>(opts_.report_dir, opts_.format, opts_.max_msgs);
}

Daemon::~Daemon() {
    stop_acceptors();
}

std::shared_ptr<Parser> Daemon::make_parser(const std::string& proto) const {
    if (proto == "Wayland") return std::make_shared<WaylandParser>(db_);
    return std::make_shared<X11Parser>();
}

std::string Daemon::pidfile_path() const {
    return opts_.runtime_dir + "/wayland-track/daemon.pid";
}

void Daemon::write_pidfile() {
    mkdir_p(opts_.runtime_dir + "/wayland-track", 0755);
    FILE* f = fopen(pidfile_path().c_str(), "w");
    if (!f) return;
    fprintf(f, "%d\n", getpid());
    fclose(f);
}

int Daemon::run() {
    {
        std::string s;
        if (read_file_str(pidfile_path(), s)) {
            int old = atoi(trim_str(s).c_str());
            if (old > 0 && kill(old, 0) == 0) {
                logf("已有守护进程在运行 (pid=%d)，请先执行 wayland-track restore 或等待其退出", old);
                return 1;
            }
        }
    }
    if (path_exists(takeover::state_path(opts_.runtime_dir))) {
        auto msgs = takeover::restore(opts_.runtime_dir);
        for (auto& m : msgs) logf("清理上次残留: %s", m.c_str());
    }
    std::vector<Target> targets = takeover::discover(opts_.runtime_dir, !opts_.no_wayland, !opts_.no_x11);
    if (!opts_.displays.empty()) {
        std::vector<Target> filtered;
        for (auto& t : targets)
            for (auto& want : opts_.displays)
                if (t.display == want || t.proxy == want) {
                    filtered.push_back(t);
                    break;
                }
        targets = filtered;
    }
    if (targets.empty()) {
        logf("没有发现可接管的 Wayland/X11 socket（RuntimeDir=%s）", opts_.runtime_dir.c_str());
        return 1;
    }

    std::vector<int> accept_fds;
    TrackState st;
    st.pid = getpid();
    st.report_dir = opts_.report_dir;
    for (auto& t : targets) {
        int fd = takeover::takeover(t);
        if (fd < 0) {
            logf("跳过 %s: 接管失败", t.proxy.c_str());
            continue;
        }
        targets_.push_back(t);
        accept_fds.push_back(fd);
        st.targets = targets_;
        takeover::save_state(opts_.runtime_dir, st);
        logf("已接管 %s [%s]，真实 socket -> %s", t.proxy.c_str(), t.proto.c_str(), t.real.c_str());
    }
    if (accept_fds.empty()) {
        logf("所有 socket 接管均失败，未开始监控");
        return 1;
    }
    write_pidfile();

    for (size_t i = 0; i < targets_.size(); i++) spawn_acceptor(accept_fds[i], targets_[i]);
    for (size_t i = 0; i < targets_.size(); i++) {
        if (targets_[i].proto != "X11") continue;
        int fda = takeover::listen_abstract(targets_[i].proxy);
        if (fda >= 0) {
            logf("已额外接管 abstract socket @%s，X11 客户端将完整经过代理", targets_[i].proxy.c_str());
            spawn_acceptor(fda, targets_[i]);
        } else if (errno == EADDRINUSE) {
            logf("警告: X server 同时监听 abstract socket @%s，X11 客户端可能绕过代理；如需可靠监控 X11 应用请使用 launch 模式",
                 targets_[i].proxy.c_str());
        }
    }
    logf("守护进程运行中 (pid=%d)，报告目录: %s", getpid(), opts_.report_dir.c_str());

    int elapsed = 0;
    while (!stop_.load()) {
        usleep(200 * 1000);
        if (g_sig_stop || stop_.load()) break;
        elapsed += 200;
        if (elapsed >= 2000) {
            elapsed = 0;
            tracker_->sweep();
        }
    }
    shutdown();
    return 0;
}

void Daemon::request_stop() {
    stop_ = true;
}

void Daemon::finalize_now() {
    tracker_->finalize_all();
}

void Daemon::spawn_acceptor(int listen_fd, const Target& t) {
    listen_fds_.push_back(listen_fd);
    acceptors_.emplace_back([this, listen_fd, t]() { this->accept_loop(listen_fd, t); });
}

void Daemon::stop_acceptors() {
    stop_ = true;
    if (acceptors_.empty()) return;
    for (int fd : listen_fds_) ::shutdown(fd, SHUT_RDWR);
    for (auto& th : acceptors_)
        if (th.joinable()) th.join();
    acceptors_.clear();
    for (int fd : listen_fds_) close(fd);
    listen_fds_.clear();
}

void Daemon::shutdown() {
    if (done_.exchange(true)) return;
    stop_acceptors();
    tracker_->finalize_all();
    auto msgs = takeover::restore(opts_.runtime_dir);
    for (auto& m : msgs) logf("%s", m.c_str());
    unlink(pidfile_path().c_str());
    logf("守护进程已退出，socket 已恢复");
}

void Daemon::accept_loop(int listen_fd, const Target& t) {
    while (!stop_.load() && !done_.load()) {
        struct pollfd pfd{listen_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 250);
        if (pr <= 0) continue;
        if (pfd.revents & POLLNVAL) break;
        while (true) {
            int cfd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                break;
            }
            handle_conn(cfd, t);
        }
    }
}

static int connect_unix_path(const std::string& path);

void Daemon::handle_conn(int client_fd, const Target& t) {
    int pid = 0, uid = 0, gid = 0;
    if (!takeover::peer_cred(client_fd, pid, uid, gid)) {
        logf("获取对端凭据失败");
        close(client_fd);
        return;
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
        close(client_fd);
        return;
    }

    auto parser = make_parser(t.proto);
    uint64_t conn_id = tracker_->open_conn(pid, t.proto, t.proxy, ai, parser);

    auto ctx = std::make_shared<PumpCtx>();
    ctx->client_fd = client_fd;
    ctx->real_fd = real_fd;
    ctx->parser = parser;
    ctx->tracker = tracker_;
    ctx->conn_id = conn_id;
    ctx->remaining = std::make_shared<std::atomic<int>>(2);

    std::thread(relay::pump, client_fd, real_fd, relay::DIR_C2S, ctx).detach();
    std::thread(relay::pump, real_fd, client_fd, relay::DIR_S2C, ctx).detach();
}

static int connect_unix_path(const std::string& path) {
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
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
    }
    return fd;
}

static int pick_x_proxy_display() {
    for (int n = 90; n < 100; n++) {
        if (!is_socket_path(fmt("/tmp/.X11-unix/X%d", n))) return n;
    }
    return -1;
}

int run_launch(const LaunchOptions& opts) {
    if (opts.command.empty()) {
        logf("launch 需要指定要启动的命令");
        return 1;
    }
    Daemon d(opts.opts);
    logf("launch 模式: %s", join_str(opts.command, " ").c_str());

    std::vector<Target> targets;
    std::vector<int> listen_fds;
    std::vector<std::pair<int, Target>> acceptor_pairs;
    std::string proxy_dir;
    std::string env_wayland, env_display;

    if (!opts.opts.no_wayland) {
        const char* wd = getenv("WAYLAND_DISPLAY");
        std::string wayland_display = wd ? wd : "wayland-0";
        std::string real = wayland_display;
        if (!real.empty() && real[0] != '/') real = opts.opts.runtime_dir + "/" + real;
        if (!is_socket_path(real)) {
            logf("警告: Wayland socket 不存在: %s", real.c_str());
        } else {
            proxy_dir = fmt("%s/wayland-track-launch-%d", opts.opts.runtime_dir.c_str(), getpid());
            mkdir_p(proxy_dir, 0700);
            std::string proxy_sock = proxy_dir + "/wayland-0";
            int fd = takeover::listen_fs(proxy_sock);
            if (fd < 0) {
                logf("创建 Wayland 代理监听失败");
            } else {
                targets.push_back({"Wayland", proxy_sock, real, "wayland-0", false});
                acceptor_pairs.push_back({fd, targets.back()});
                env_wayland = proxy_dir + "/wayland-0";
                logf("Wayland 代理: %s -> %s", proxy_sock.c_str(), real.c_str());
            }
        }
    }

    if (!opts.opts.no_x11) {
        const char* dp = getenv("DISPLAY");
        std::string display = dp ? dp : "";
        if (display.empty() || display.find(':') == std::string::npos) {
            logf("警告: DISPLAY 未设置或无法解析，跳过 X11 代理");
        } else {
            std::string num = display.substr(display.find(':') + 1);
            size_t dot = num.find('.');
            if (dot != std::string::npos) num = num.substr(0, dot);
            int n = atoi(num.c_str());
            if (n < 0) {
                logf("警告: DISPLAY=%s 无法解析", display.c_str());
            } else {
                std::string real = fmt("/tmp/.X11-unix/X%d", n);
                int proxy_n = pick_x_proxy_display();
                if (proxy_n < 0) {
                    logf("没有可用的 X11 代理显示号");
                } else {
                    std::string proxy_path = fmt("/tmp/.X11-unix/X%d", proxy_n);
                    int fd = takeover::listen_fs(proxy_path);
                    int fda = takeover::listen_abstract(proxy_path);
                    if (fd < 0 || fda < 0) {
                        logf("创建 X11 代理监听失败");
                        if (fd >= 0) close(fd);
                        if (fda >= 0) close(fda);
                    } else {
                        targets.push_back({"X11", proxy_path, real, fmt("X%d", proxy_n), false});
                        acceptor_pairs.push_back({fd, targets.back()});
                        acceptor_pairs.push_back({fda, targets.back()});
                        env_display = fmt(":%d", proxy_n);
                        logf("X11 代理: :%d(fs+abstract) -> %s", proxy_n, real.c_str());
                    }
                }
            }
        }
    }

    for (auto& pr : acceptor_pairs) d.spawn_acceptor(pr.first, pr.second);

    std::vector<std::string> env_strings;
    for (char** e = environ; *e; e++) {
        std::string kv(*e);
        if (!env_wayland.empty() && kv.rfind("WAYLAND_DISPLAY=", 0) == 0) continue;
        if (!env_display.empty() && kv.rfind("DISPLAY=", 0) == 0) continue;
        env_strings.push_back(kv);
    }
    if (!env_wayland.empty()) env_strings.push_back("WAYLAND_DISPLAY=" + env_wayland);
    if (!env_display.empty()) env_strings.push_back("DISPLAY=" + env_display);
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<char*> argv;
    for (auto& c : opts.command) argv.push_back(const_cast<char*>(c.c_str()));
    argv.push_back(nullptr);

    pid_t child = fork();
    if (child < 0) {
        logf("fork 失败: %s", strerror(errno));
        return 1;
    }
    if (child == 0) {
        execvpe(opts.command[0].c_str(), argv.data(), envp.data());
        fprintf(stderr, "exec %s 失败: %s\n", opts.command[0].c_str(), strerror(errno));
        _exit(127);
    }
    logf("应用已启动 pid=%d，报告将在应用退出后写入 %s", child, opts.opts.report_dir.c_str());

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) break;
    }

    d.stop_acceptors();
    for (auto& t : targets) {
        if (t.proxy.find("wayland-track-launch") != std::string::npos) {
            unlink(t.proxy.c_str());
            if (!proxy_dir.empty()) rmdir(proxy_dir.c_str());
        } else if (t.proxy.rfind("/tmp/.X11-unix/X9", 0) == 0) {
            unlink(t.proxy.c_str());
        }
    }
    usleep(300 * 1000);
    d.finalize_now();
    if (WIFEXITED(status))
        logf("应用正常退出 (%d)", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        logf("应用被信号终止 (%d)", WTERMSIG(status));
    logf("报告已写入 %s", opts.opts.report_dir.c_str());
    return 0;
}
