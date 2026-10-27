#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "proto_db.hpp"
#include "report.hpp"
#include "takeover.hpp"

struct PumpCtx;

struct DaemonOptions {
    std::string runtime_dir;
    std::string report_dir;
    std::vector<std::string> protocols_dirs;
    bool no_wayland = false;
    bool no_x11 = false;
    std::string format = "both";
    int max_msgs = 50000;
    std::vector<std::string> displays;
};

struct LaunchOptions {
    DaemonOptions opts;
    std::vector<std::string> command;
};

class Daemon {
public:
    explicit Daemon(const DaemonOptions& opts);
    ~Daemon();
    int run();
    void request_stop();
    void finalize_now();
    int active_conns_for_test() const { return tracker_->active_conns(); }
    void spawn_acceptor(int listen_fd, const Target& t);
    void stop_acceptors();

private:
    void accept_loop(int listen_fd, const Target& t);
    void handle_conn(int client_fd, const Target& t);
    void shutdown();
    std::shared_ptr<Parser> make_parser(const std::string& proto) const;

public:
    std::shared_ptr<const ProtoDB> database() const { return db_; }

private:
    std::string pidfile_path() const;
    void write_pidfile();

    DaemonOptions opts_;
    std::shared_ptr<ProtoDB> db_;
    std::shared_ptr<Tracker> tracker_;
    std::vector<Target> targets_;
    std::vector<int> listen_fds_;
    std::vector<std::thread> acceptors_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{false};
};

int run_launch(const LaunchOptions& opts);

struct XwaylandOptions;

int run_xwayland(const XwaylandOptions& opts);

void install_signal_handlers();
