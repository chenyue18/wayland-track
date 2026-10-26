#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <dirent.h>
#include <cstring>
#include <fstream>
#include <thread>

#include "daemon.hpp"
#include "logger.hpp"
#include "test_util.hpp"
#include "util.hpp"

namespace {

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 24) & 0xff);
}

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
}

void put_str(std::vector<uint8_t>& v, const std::string& s) {
    put_u32(v, uint32_t(s.size() + 1));
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
    while (v.size() % 4) v.push_back(0);
}

std::vector<uint8_t> wl_msg(uint32_t obj, uint16_t opcode, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> v;
    put_u32(v, obj);
    put_u16(v, opcode);
    put_u16(v, uint16_t(8 + payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

int listen_unix(const std::string& path) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    bind(fd, (struct sockaddr*)&sa, sizeof(sa));
    listen(fd, 8);
    return fd;
}

int connect_unix(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool wait_for(const std::string& path, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        if (path_exists(path)) return true;
        usleep(20 * 1000);
    }
    return false;
}

}  // namespace

void run_test_integration() {
    std::string tmp = "/tmp/opencode/wt-cpp-test";
    std::string runtime_dir = tmp + "/run";
    std::string report_dir = tmp + "/reports";
    mkdir_p(runtime_dir, 0755);

    std::string real_path = runtime_dir + "/wayland-0";
    int srv = listen_unix(real_path);

    std::vector<uint8_t> received;
    std::thread server_thread([&srv, &received]() {
        int c = accept(srv, nullptr, nullptr);
        if (c < 0) return;
        char buf[8192];
        ssize_t n = recv(c, buf, sizeof buf, 0);
        if (n > 0) received.assign(buf, buf + n);
        std::vector<uint8_t> resp;
        {
            std::vector<uint8_t> payload;
            put_u32(payload, 1);
            put_str(payload, "wl_compositor");
            put_u32(payload, 5);
            auto m = wl_msg(2, 0, payload);
            resp.insert(resp.end(), m.begin(), m.end());
        }
        {
            std::vector<uint8_t> payload;
            put_u32(payload, 9);
            auto m = wl_msg(1, 1, payload);
            resp.insert(resp.end(), m.begin(), m.end());
        }
        send(c, resp.data(), resp.size(), 0);
        close(c);
    });

    DaemonOptions opts;
    opts.runtime_dir = runtime_dir;
    opts.report_dir = report_dir;
    opts.no_x11 = true;
    opts.format = "both";
    opts.max_msgs = 100;

    Daemon d(opts);
    std::thread daemon_thread([&d]() { d.run(); });

    CHECK(wait_for(real_path + ".wt-real", 5000));

    int cl = connect_unix(real_path);
    CHECK(cl >= 0);
    std::vector<uint8_t> req = wl_msg(1, 1, [&] {
        std::vector<uint8_t> v;
        put_u32(v, 2);
        return v;
    }());
    send(cl, req.data(), req.size(), 0);

    char buf[8192];
    ssize_t rn = recv(cl, buf, sizeof buf, 0);
    CHECK(rn > 0);
    close(cl);
    server_thread.join();
    CHECK(received.size() >= 12);

    for (int i = 0; i < 50 && d.active_conns_for_test() > 0; i++) usleep(20 * 1000);
    d.finalize_now();
    d.request_stop();
    daemon_thread.join();
    close(srv);

    std::vector<std::string> mds, jsons;
    if (DIR* d = opendir(report_dir.c_str())) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string n = e->d_name;
            if (n.size() > 3 && n.substr(n.size() - 3) == ".md") mds.push_back(report_dir + "/" + n);
            if (n.size() > 5 && n.substr(n.size() - 5) == ".json") jsons.push_back(report_dir + "/" + n);
        }
        closedir(d);
    }
    CHECK(!mds.empty());
    CHECK(!jsons.empty());
    if (!mds.empty()) {
        std::ifstream f(mds[0]);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK_CONTAIN(content, "get_registry");
        CHECK_CONTAIN(content, "wl_compositor");
        CHECK_CONTAIN(content, "global");
        CHECK_CONTAIN(content, "delete_id");
    }
}
