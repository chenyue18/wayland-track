#include "procinfo.hpp"

#include <unistd.h>

#include <cstdio>

ProcInfo procinfo_read(int pid) {
    ProcInfo info;
    info.pid = pid;
    char buf[128];
    snprintf(buf, sizeof buf, "/proc/%d/comm", pid);
    std::string s;
    if (read_file_str(buf, s)) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        info.comm = s;
    }
    snprintf(buf, sizeof buf, "/proc/%d/cmdline", pid);
    if (read_file_str(buf, s)) {
        std::string out;
        for (char c : s) out.push_back(c == '\0' ? ' ' : c);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        info.cmdline = out;
    }
    snprintf(buf, sizeof buf, "/proc/%d/exe", pid);
    char link[4096];
    ssize_t n = readlink(buf, link, sizeof(link) - 1);
    if (n > 0) {
        link[n] = 0;
        info.exe = link;
    }
    if (info.comm.empty()) info.comm = "unknown";
    return info;
}

bool procinfo_alive(int pid) {
    char buf[64];
    snprintf(buf, sizeof buf, "/proc/%d/stat", pid);
    std::string s;
    if (!read_file_str(buf, s)) return false;
    size_t rparen = s.rfind(')');
    if (rparen == std::string::npos || rparen + 2 >= s.size()) return false;
    char state = s[rparen + 2];
    return state != 'Z' && state != 'X';
}
