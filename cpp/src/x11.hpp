#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "report.hpp"
#include "x11_tables.hpp"

namespace x11 {
enum { DIR_C2S = 0, DIR_S2C = 1 };
enum { PH_SETUP = 0, PH_STREAM = 1, PH_DEAD = 2 };
}

class X11Parser : public Parser {
public:
    X11Parser();
    FeedResult feed(int dir, const uint8_t* data, size_t len, int fd_count, TimePoint t) override;
    std::map<std::string, std::string> summary() override;

private:
    struct ExtEvent {
        uint8_t first;
        std::string name;
    };

    bool step_setup(int dir, const char* dir_name, TimePoint t, int& consumed, Msg& m);
    bool step_request(const char* dir_name, int fd_count, TimePoint t, int& consumed, Msg& m);
    bool step_reply(const char* dir_name, TimePoint t, int& consumed, Msg& m);
    std::string decode_request(int op, const uint8_t* b, size_t len) const;
    std::string atom_name(uint32_t id) const;
    std::string ext_event_name(uint8_t code) const;
    uint16_t u16(const uint8_t* b, size_t off) const;
    uint32_t u32(const uint8_t* b, size_t off) const;

    std::mutex mu_;
    bool big_endian_ = false;
    std::vector<uint8_t> buf_[2];
    int phase_[2] = {x11::PH_SETUP, x11::PH_SETUP};
    std::map<uint8_t, std::string> ext_name_;
    std::vector<ExtEvent> ext_events_;
    std::map<uint16_t, std::string> pending_;
    std::map<uint32_t, std::string> atoms_;
    uint16_t seq_ = 0;
    std::string vendor_;
    int major_ = 0, minor_ = 0;
    uint32_t release_ = 0;
    std::vector<std::string> ext_list_;
    bool parse_err_[2] = {false, false};
};
