#include "util.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

std::string fmt(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string out;
    if (n > 0) {
        out.resize(size_t(n));
        vsnprintf(out.data(), size_t(n) + 1, f, ap2);
    }
    va_end(ap2);
    return out;
}

static std::string fmt_time_impl(const TimePoint& t, bool with_date) {
    std::time_t tt = Clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000);
    if (ms < 0) ms += 1000;
    char buf[64];
    if (with_date) {
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
        return fmt("%s.%03d", buf, ms);
    }
    strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    return fmt("%s.%03d", buf, ms);
}

std::string fmt_time_full(const TimePoint& t) { return fmt_time_impl(t, true); }

std::string fmt_stamp(const TimePoint& t) {
    std::time_t tt = Clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}
std::string fmt_time_clock(const TimePoint& t) { return fmt_time_impl(t, false); }

std::string sanitize_name(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out.push_back(c);
        else
            out.push_back('_');
    }
    while (!out.empty() && (out.front() == '_' || out.front() == '.' || out.front() == '-')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '_' || out.back() == '.' || out.back() == '-')) out.pop_back();
    if (out.size() > 48) out.resize(48);
    return out.empty() ? "unknown" : out;
}

std::string to_abs_path(const std::string& p) {
    if (!p.empty() && p[0] == '/') return p;
    char buf[4096];
    if (getcwd(buf, sizeof buf) == nullptr) return p;
    return std::string(buf) + "/" + p;
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool is_socket_path(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISSOCK(st.st_mode);
}

bool read_file_str(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool mkdir_p(const std::string& path, int mode) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '/' && i > 0) {
            cur = path.substr(0, i);
            ::mkdir(cur.c_str(), mode);
        }
    }
    ::mkdir(path.c_str(), mode);
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> split_str(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join_str(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::string trim_str(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    char buf[8];
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(char(c));
            }
        }
    }
    return out;
}

std::string md_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\n' || c == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}
