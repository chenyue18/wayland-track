#pragma once

#include <string>
#include <vector>

#include "util.hpp"

struct Target {
    std::string proto;
    std::string proxy;
    std::string real;
    std::string display;
    bool abstract = false;
};

struct TrackState {
    int pid = 0;
    std::string report_dir;
    std::vector<Target> targets;
};

namespace takeover {

std::string state_path(const std::string& runtime_dir);
TrackState load_state(const std::string& runtime_dir);
bool save_state(const std::string& runtime_dir, const TrackState& st);
void remove_state(const std::string& runtime_dir);

std::vector<Target> discover(const std::string& runtime_dir, bool want_wayland, bool want_x11);

int takeover(const Target& t);
int listen_fs(const std::string& path);
int listen_fs_mode(const std::string& path, int mode);
int listen_abstract(const std::string& path);
std::vector<std::string> restore(const std::string& runtime_dir);

bool peer_cred(int fd, int& pid, int& uid, int& gid);

}  // namespace takeover
