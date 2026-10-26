#include "takeover.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>

#include "logger.hpp"

static bool is_num_str(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

namespace takeover {

std::string state_path(const std::string& runtime_dir) {
    return runtime_dir + "/wayland-track/state.json";
}

static std::string jstr(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

bool save_state(const std::string& runtime_dir, const TrackState& st) {
    std::string p = state_path(runtime_dir);
    mkdir_p(runtime_dir + "/wayland-track", 0755);
    std::string o = "{\n  \"daemon_pid\": " + fmt("%d", st.pid) + ",\n  \"report_dir\": " + jstr(st.report_dir) +
                    ",\n  \"takeovers\": [\n";
    for (size_t i = 0; i < st.targets.size(); i++) {
        const Target& t = st.targets[i];
        o += fmt("    {\"proto\": %s, \"proxy\": %s, \"real\": %s, \"display\": %s, \"abstract\": %s}%s\n",
                 jstr(t.proto).c_str(), jstr(t.proxy).c_str(), jstr(t.real).c_str(), jstr(t.display).c_str(),
                 t.abstract ? "true" : "false", (i + 1 < st.targets.size()) ? "," : "");
    }
    o += "  ]\n}\n";
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return false;
    fwrite(o.data(), 1, o.size(), f);
    fclose(f);
    return true;
}

static std::string extract_json_string(const std::string& src, size_t pos, size_t& end_out) {
    size_t q1 = src.find('"', pos);
    if (q1 == std::string::npos) return "";
    std::string out;
    size_t i = q1 + 1;
    while (i < src.size()) {
        char c = src[i];
        if (c == '\\' && i + 1 < src.size()) {
            char n = src[i + 1];
            if (n == '"') out += '"';
            else if (n == '\\') out += '\\';
            else if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else out += n;
            i += 2;
            continue;
        }
        if (c == '"') break;
        out += c;
        i++;
    }
    end_out = i + 1;
    return out;
}

TrackState load_state(const std::string& runtime_dir) {
    TrackState st;
    std::string s;
    if (!read_file_str(state_path(runtime_dir), s)) return st;
    st.pid = 0;
    size_t pid_pos = s.find("\"daemon_pid\"");
    if (pid_pos != std::string::npos) {
        st.pid = atoi(s.c_str() + s.find(':', pid_pos) + 1);
    }
    size_t rd_pos = s.find("\"report_dir\"");
    if (rd_pos != std::string::npos) {
        size_t colon = s.find(':', rd_pos);
        st.report_dir = extract_json_string(s, colon, rd_pos);
    }
    size_t pos = 0;
    while ((pos = s.find("\"proto\"", pos)) != std::string::npos) {
        Target t;
        size_t end;
        size_t colon = s.find(':', pos);
        t.proto = extract_json_string(s, colon, end);
        size_t pp = s.find("\"proxy\"", pos);
        if (pp == std::string::npos) break;
        t.proxy = extract_json_string(s, s.find(':', pp), end);
        size_t rp = s.find("\"real\"", pos);
        if (rp == std::string::npos) break;
        t.real = extract_json_string(s, s.find(':', rp), end);
        size_t dp = s.find("\"display\"", pos);
        if (dp != std::string::npos && dp < pos + 200) t.display = extract_json_string(s, s.find(':', dp), end);
        size_t ap = s.find("\"abstract\"", pos);
        if (ap != std::string::npos) {
            size_t colon = s.find(':', ap);
            size_t tru = s.find("true", colon);
            size_t fals = s.find("false", colon);
            t.abstract = tru != std::string::npos && (fals == std::string::npos || tru < fals) && tru - ap < 30;
        }
        st.targets.push_back(t);
        pos = end;
    }
    return st;
}

void remove_state(const std::string& runtime_dir) {
    unlink(state_path(runtime_dir).c_str());
}

std::vector<Target> discover(const std::string& runtime_dir, bool want_wayland, bool want_x11) {
    std::vector<Target> out;
    if (want_wayland && !runtime_dir.empty()) {
        DIR* d = opendir(runtime_dir.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                std::string name = e->d_name;
                if (name.rfind("wayland-", 0) != 0) continue;
                if (name.find("track") != std::string::npos) continue;
                if (name.size() > 8 && name.substr(name.size() - 8) == ".wt-real") continue;
                std::string full = runtime_dir + "/" + name;
                if (is_socket_path(full)) out.push_back({"Wayland", full, full + ".wt-real", name, false});
            }
            closedir(d);
        }
    }
    if (want_x11) {
        DIR* d = opendir("/tmp/.X11-unix");
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                std::string name = e->d_name;
                if (name.size() < 2 || name[0] != 'X') continue;
                if (!is_num_str(name.substr(1))) continue;
                if (name.size() > 8 && name.substr(name.size() - 8) == ".wt-real") continue;
                if (atoi(name.substr(1).c_str()) >= 90) continue;
                std::string full = "/tmp/.X11-unix/" + name;
                if (is_socket_path(full)) out.push_back({"X11", full, full + ".wt-real", name, false});
            }
            closedir(d);
        }
    }
    return out;
}

static int bind_unix(const std::string& path, bool abstract, int mode) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    socklen_t len;
    if (abstract) {
        if (path.size() > 106) {
            close(fd);
            return -1;
        }
        sa.sun_path[0] = '\0';
        memcpy(sa.sun_path + 1, path.c_str(), path.size());
        len = socklen_t(offsetof(struct sockaddr_un, sun_path) + 1 + path.size());
    } else {
        if (path.size() > 107) {
            close(fd);
            return -1;
        }
        strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        unlink(path.c_str());
        len = sizeof(sa);
    }
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), len) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        close(fd);
        return -1;
    }
    if (!abstract && mode > 0) chmod(path.c_str(), mode);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

int listen_fs(const std::string& path) {
    return bind_unix(path, false, 0777);
}

int listen_fs_mode(const std::string& path, int mode) {
    return bind_unix(path, false, mode);
}

int listen_abstract(const std::string& path) {
    return bind_unix(path, true, 0);
}

int takeover(const Target& t) {
    if (path_exists(t.real)) {
        logf("残留的真实 socket %s 已存在，请先执行 wayland-track restore", t.real.c_str());
        return -1;
    }
    mode_t mode = 0777;
    struct stat st{};
    if (stat(t.proxy.c_str(), &st) == 0) mode = st.st_mode & 0777;
    if (rename(t.proxy.c_str(), t.real.c_str()) != 0) {
        logf("重命名 %s 失败: %s", t.proxy.c_str(), strerror(errno));
        return -1;
    }
    int fd = listen_fs_mode(t.proxy, int(mode));
    if (fd < 0) {
        logf("在 %s 上创建代理监听失败: %s", t.proxy.c_str(), strerror(errno));
        rename(t.real.c_str(), t.proxy.c_str());
        return -1;
    }
    return fd;
}

static std::vector<std::string> recover_scan(const std::string& runtime_dir) {
    const char* dirs[] = {runtime_dir.c_str(), "/tmp/.X11-unix"};
    std::vector<std::string> msgs;
    bool found = false;
    for (const char* dir : dirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.size() <= 8 || name.substr(name.size() - 8) != ".wt-real") continue;
            found = true;
            std::string real = std::string(dir) + "/" + name;
            std::string proxy = real.substr(0, real.size() - 8);
            if (rename(real.c_str(), proxy.c_str()) != 0) {
                msgs.push_back(fmt("恢复 %s 失败: %s", proxy.c_str(), strerror(errno)));
            } else {
                msgs.push_back(fmt("已恢复(扫描) %s", proxy.c_str()));
            }
        }
        closedir(d);
    }
    if (!found) msgs.push_back("没有找到接管状态文件，也无需恢复");
    return msgs;
}

std::vector<std::string> restore(const std::string& runtime_dir) {
    TrackState st = load_state(runtime_dir);
    if (st.targets.empty() && st.pid == 0 && st.report_dir.empty()) {
        return recover_scan(runtime_dir);
    }
    std::vector<std::string> msgs;
    for (auto& t : st.targets) {
        if (t.abstract) continue;
        if (!path_exists(t.real)) {
            msgs.push_back(t.proxy + ": 无需恢复（真实 socket 已在原位）");
            continue;
        }
        if (rename(t.real.c_str(), t.proxy.c_str()) != 0)
            msgs.push_back(fmt("恢复 %s 失败: %s", t.proxy.c_str(), strerror(errno)));
        else
            msgs.push_back(fmt("已恢复 %s", t.proxy.c_str()));
    }
    remove_state(runtime_dir);
    auto left = recover_scan(runtime_dir);
    for (auto& m : left)
        if (m.rfind("没有找到", 0) != 0) msgs.push_back(m);
    return msgs;
}

bool peer_cred(int fd, int& pid, int& uid, int& gid) {
    struct ucred cr{};
    socklen_t len = sizeof(cr);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return false;
    pid = cr.pid;
    uid = cr.uid;
    gid = cr.gid;
    return true;
}

}  // namespace takeover
