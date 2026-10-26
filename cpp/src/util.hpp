#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));

std::string fmt_time_full(const TimePoint& t);
std::string fmt_stamp(const TimePoint& t);
std::string fmt_time_clock(const TimePoint& t);

std::string sanitize_name(const std::string& s);
std::string to_abs_path(const std::string& p);

bool read_file_str(const std::string& path, std::string& out);
bool path_exists(const std::string& path);
bool is_socket_path(const std::string& path);
bool mkdir_p(const std::string& path, int mode = 0755);

std::vector<std::string> split_str(const std::string& s, char sep);
std::string join_str(const std::vector<std::string>& v, const std::string& sep);
std::string trim_str(const std::string& s);

inline uint16_t rd_le16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t rd_le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint16_t rd_be16(const uint8_t* p) {
    return uint16_t(p[0]) << 8 | uint16_t(p[1]);
}
inline uint32_t rd_be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

std::string json_escape(const std::string& s);
std::string md_escape(const std::string& s);
