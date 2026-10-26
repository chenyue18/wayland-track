#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "proto_db.hpp"
#include "report.hpp"

namespace wayland {
enum { DIR_C2S = 0, DIR_S2C = 1 };
}

class WaylandParser : public Parser {
public:
    explicit WaylandParser(std::shared_ptr<const ProtoDB> db);
    FeedResult feed(int dir, const uint8_t* data, size_t len, int fd_count, TimePoint t) override;
    std::map<std::string, std::string> summary() override;

private:
    Decoded decode(int dir, const std::string& iface, uint32_t obj_id, int opcode, const uint8_t* payload, size_t len);
    std::string decode_arg(int dir, ArgReader& rd, const WArg& a, const std::string& iface);
    std::string obj_name(uint32_t id) const;

    std::mutex mu_;
    std::shared_ptr<const ProtoDB> db_;
    std::map<uint32_t, std::string> objects_;
    std::vector<uint8_t> buf_[2];
    int fd_avail_[2] = {0, 0};
    int fd_debt_[2] = {0, 0};
    int fd_total_[2] = {0, 0};
    std::map<uint32_t, std::string> globals_;
    std::vector<std::string> binds_;
    bool parse_err_[2] = {false, false};
};
