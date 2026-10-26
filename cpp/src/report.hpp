#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "util.hpp"

struct Msg {
    TimePoint time;
    std::string dir;
    std::string iface;
    std::string name;
    std::string object;
    std::string detail;
    int bytes = 0;
};

struct IfaceStat {
    int sent = 0;
    int recv = 0;
};

struct AppInfo {
    int pid = 0;
    int uid = 0;
    int gid = 0;
    std::string comm;
    std::string exe;
    std::string cmdline;
};

struct ConnReport {
    int id = 0;
    std::string proto;
    std::string socket;
    TimePoint start;
    TimePoint end;
    bool closed = false;
    int msg_count = 0;
    int logged = 0;
    bool truncated = false;
    std::map<std::string, IfaceStat> stats;
    std::map<std::string, std::string> meta;
    std::vector<Msg> msgs;
};

struct Report {
    AppInfo app;
    TimePoint generated_at;
    TimePoint first_seen;
    std::vector<ConnReport> connections;
};

struct Decoded {
    std::string name;
    std::string detail;
};

struct FeedResult {
    std::vector<Msg> msgs;
    int bytes = 0;
};

class ArgReader {
public:
    ArgReader(const uint8_t* b, size_t n) : b_(b), n_(n) {}
    bool u32(uint32_t& v);
    bool i32(int32_t& v);
    bool str(std::string& v);
    bool skip_bytes(size_t n);
    size_t off() const { return off_; }

private:
    const uint8_t* b_;
    size_t n_;
    size_t off_ = 0;
};

class Parser {
public:
    virtual ~Parser() = default;
    virtual FeedResult feed(int dir, const uint8_t* data, size_t len, int fd_count, TimePoint t) = 0;
    virtual std::map<std::string, std::string> summary() = 0;
};

bool write_json_report(const std::string& path, const Report& rep);
bool write_markdown_report(const std::string& path, const Report& rep);

class Tracker {
public:
    Tracker(std::string dir, std::string format, int max_msgs);

    uint64_t open_conn(int pid, const std::string& proto, const std::string& socket, const AppInfo& app,
                       std::shared_ptr<Parser> parser);
    void add_msgs(uint64_t conn_id, std::vector<Msg> msgs);
    void close_conn(uint64_t conn_id);
    void sweep();
    void finalize_all();
    int active_conns() const;

private:
    struct ConnData;
    struct Session;

    void finalize_locked(int pid);
    Report build_report(const Session& s) const;
    std::string write_report(const Report& rep) const;

    struct ConnData {
        uint64_t id = 0;
        int pid = 0;
        std::string proto;
        std::string socket;
        TimePoint start, end;
        bool closed = false;
        int count = 0, logged = 0, dropped = 0;
        std::map<std::string, IfaceStat> stats;
        std::shared_ptr<Parser> parser;
        std::vector<Msg> msgs;
    };
    struct Session {
        int pid = 0;
        AppInfo app;
        std::vector<std::shared_ptr<ConnData>> conns;
        TimePoint first, last;
        int open_conns = 0;
    };

    mutable std::mutex mu_;
    std::string dir_;
    std::string format_;
    int max_msgs_;
    std::map<int, Session> sessions_;
    std::map<uint64_t, std::shared_ptr<ConnData>> conns_;
    uint64_t next_conn_id_ = 1;
    int next_conn_n_ = 0;
};
