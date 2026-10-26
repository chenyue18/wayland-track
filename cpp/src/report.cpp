#include "report.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "logger.hpp"
#include "procinfo.hpp"

bool ArgReader::u32(uint32_t& v) {
    if (off_ + 4 > n_) return false;
    v = rd_le32(b_ + off_);
    off_ += 4;
    return true;
}

bool ArgReader::i32(int32_t& v) {
    uint32_t u;
    if (!u32(u)) return false;
    v = int32_t(u);
    return true;
}

bool ArgReader::str(std::string& v) {
    uint32_t n;
    if (!u32(n)) return false;
    if (n > (1u << 20) || off_ + n > n_) return false;
    v.assign(reinterpret_cast<const char*>(b_ + off_), n);
    off_ += n;
    off_ = (off_ + 3) & ~size_t(3);
    size_t z = v.find('\0');
    if (z != std::string::npos) v.resize(z);
    return true;
}

bool ArgReader::skip_bytes(size_t n) {
    if (off_ + n > n_) return false;
    off_ += n;
    off_ = (off_ + 3) & ~size_t(3);
    return true;
}

Tracker::Tracker(std::string dir, std::string format, int max_msgs)
    : dir_(std::move(dir)), format_(format.empty() ? "both" : std::move(format)), max_msgs_(max_msgs > 0 ? max_msgs : 50000) {
    mkdir_p(dir_, 0755);
}

uint64_t Tracker::open_conn(int pid, const std::string& proto, const std::string& socket, const AppInfo& app,
                            std::shared_ptr<Parser> parser) {
    std::lock_guard<std::mutex> lk(mu_);
    TimePoint now = Clock::now();
    Session& s = sessions_[pid];
    if (s.conns.empty()) {
        s.pid = pid;
        s.app = app;
        s.first = now;
    }
    s.last = now;
    s.open_conns++;
    auto c = std::make_shared<ConnData>();
    c->id = next_conn_id_++;
    c->pid = pid;
    c->proto = proto;
    c->socket = socket;
    c->start = now;
    c->parser = std::move(parser);
    s.conns.push_back(c);
    conns_[c->id] = c;
    logf("连接 #%llu: pid=%d %s [%s] via %s", (unsigned long long)c->id, pid, app.comm.c_str(), proto.c_str(),
         socket.c_str());
    return c->id;
}

void Tracker::add_msgs(uint64_t conn_id, std::vector<Msg> msgs) {
    if (msgs.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return;
    ConnData& c = *it->second;
    TimePoint now = Clock::now();
    auto sit = sessions_.find(c.pid);
    if (sit != sessions_.end()) sit->second.last = now;
    for (auto& m : msgs) {
        c.count++;
        IfaceStat& st = c.stats[m.iface];
        if (m.dir == "C→S")
            st.sent++;
        else
            st.recv++;
        if (c.logged < max_msgs_) {
            c.msgs.push_back(std::move(m));
            c.logged++;
        } else {
            c.dropped++;
        }
    }
}

void Tracker::close_conn(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return;
    std::shared_ptr<ConnData> c = it->second;
    TimePoint now = Clock::now();
    c->closed = true;
    c->end = now;
    auto sit = sessions_.find(c->pid);
    if (sit != sessions_.end()) {
        Session& s = sit->second;
        s.open_conns--;
        s.last = now;
        if (s.open_conns <= 0 && !procinfo_alive(c->pid)) {
            finalize_locked(c->pid);
        }
    }
    conns_.erase(it);
    logf("连接 #%llu 关闭: pid=%d [%s] 共 %d 条消息", (unsigned long long)conn_id, c->pid, c->proto.c_str(), c->count);
}

void Tracker::sweep() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> dead;
    for (auto& kv : sessions_)
        if (!procinfo_alive(kv.first)) dead.push_back(kv.first);
    for (int pid : dead) finalize_locked(pid);
}

void Tracker::finalize_all() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> pids;
    for (auto& kv : sessions_) pids.push_back(kv.first);
    for (int pid : pids) finalize_locked(pid);
}

int Tracker::active_conns() const {
    std::lock_guard<std::mutex> lk(mu_);
    return int(conns_.size());
}

void Tracker::finalize_locked(int pid) {
    auto it = sessions_.find(pid);
    if (it == sessions_.end()) return;
    Report rep = build_report(it->second);
    sessions_.erase(it);
    std::string path = write_report(rep);
    logf("已生成报告: %s", path.c_str());
}

Report Tracker::build_report(const Session& s) const {
    Report rep;
    rep.app = s.app;
    rep.generated_at = Clock::now();
    rep.first_seen = s.first;
    for (auto& c : s.conns) {
        ConnReport cr;
        cr.id = int(c->id);
        cr.proto = c->proto;
        cr.socket = c->socket;
        cr.start = c->start;
        cr.closed = c->closed;
        if (c->closed) cr.end = c->end;
        cr.msg_count = c->count;
        cr.logged = c->logged;
        cr.truncated = c->dropped > 0;
        cr.stats = c->stats;
        if (c->parser) cr.meta = c->parser->summary();
        cr.msgs = c->msgs;
        rep.connections.push_back(std::move(cr));
    }
    return rep;
}

static std::vector<std::string> ifaces_sorted(const std::map<std::string, IfaceStat>& stats) {
    std::vector<std::string> names;
    for (auto& kv : stats) names.push_back(kv.first);
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
        int ca = stats.at(a).sent + stats.at(a).recv;
        int cb = stats.at(b).sent + stats.at(b).recv;
        if (ca != cb) return ca > cb;
        return a < b;
    });
    return names;
}

bool write_json_report(const std::string& path, const Report& rep) {
    std::string o;
    o.reserve(1 << 16);
    o += "{\n";
    o += "  \"app\": {\"pid\": " + fmt("%d", rep.app.pid) + ", \"comm\": \"" + json_escape(rep.app.comm) +
         "\", \"exe\": \"" + json_escape(rep.app.exe) + "\", \"cmdline\": \"" + json_escape(rep.app.cmdline) +
         "\", \"uid\": " + fmt("%d", rep.app.uid) + ", \"gid\": " + fmt("%d", rep.app.gid) + "},\n";
    o += "  \"generated_at\": \"" + fmt_time_full(rep.generated_at) + "\",\n";
    o += "  \"first_seen\": \"" + fmt_time_full(rep.first_seen) + "\",\n";
    o += "  \"connections\": [\n";
    for (size_t i = 0; i < rep.connections.size(); i++) {
        const ConnReport& c = rep.connections[i];
        o += "    {\n";
        o += "      \"id\": " + fmt("%d", c.id) + ",\n";
        o += "      \"protocol\": \"" + json_escape(c.proto) + "\",\n";
        o += "      \"socket\": \"" + json_escape(c.socket) + "\",\n";
        o += "      \"start\": \"" + fmt_time_full(c.start) + "\",\n";
        o += std::string("      \"end\": ") + (c.closed ? "\"" + fmt_time_full(c.end) + "\"" : "null") + ",\n";
        o += "      \"message_count\": " + fmt("%d", c.msg_count) + ",\n";
        o += "      \"logged_messages\": " + fmt("%d", c.logged) + ",\n";
        o += std::string("      \"truncated\": ") + (c.truncated ? "true" : "false") + ",\n";
        o += "      \"interface_stats\": {";
        {
            bool first = true;
            for (auto& kv : c.stats) {
                if (!first) o += ", ";
                first = false;
                o += "\"" + json_escape(kv.first) + "\": {\"sent\": " + fmt("%d", kv.second.sent) +
                     ", \"recv\": " + fmt("%d", kv.second.recv) + "}";
            }
        }
        o += "},\n";
        o += "      \"meta\": {";
        {
            bool first = true;
            for (auto& kv : c.meta) {
                if (!first) o += ", ";
                first = false;
                o += "\"" + json_escape(kv.first) + "\": \"" + json_escape(kv.second) + "\"";
            }
        }
        o += "},\n";
        o += "      \"messages\": [\n";
        for (size_t j = 0; j < c.msgs.size(); j++) {
            const Msg& m = c.msgs[j];
            o += "        {\"time\": \"" + fmt_time_full(m.time) + "\", \"dir\": \"" + json_escape(m.dir) +
                 "\", \"interface\": \"" + json_escape(m.iface) + "\", \"name\": \"" + json_escape(m.name) +
                 "\", \"object\": \"" + json_escape(m.object) + "\", \"detail\": \"" + json_escape(m.detail) +
                 "\", \"bytes\": " + fmt("%d", m.bytes) + "}";
            if (j + 1 < c.msgs.size()) o += ",";
            o += "\n";
        }
        o += "      ]\n";
        o += "    }";
        if (i + 1 < rep.connections.size()) o += ",";
        o += "\n";
    }
    o += "  ]\n}\n";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << o;
    return bool(f);
}

bool write_markdown_report(const std::string& path, const Report& rep) {
    std::string b;
    b.reserve(1 << 16);
    b += "# 应用图形协议使用记录\n\n";
    b += "> 由 wayland-track (C++) 自动生成。应用退出后，记录其在 Wayland / X11 连接中使用的协议接口与消息参数。\n\n";
    b += "## 应用信息\n\n| 项 | 值 |\n|---|---|\n";
    b += fmt("| 应用 | %s (pid %d) |\n", md_escape(rep.app.comm).c_str(), rep.app.pid);
    if (!rep.app.exe.empty()) b += fmt("| 可执行文件 | `%s` |\n", md_escape(rep.app.exe).c_str());
    if (!rep.app.cmdline.empty()) b += fmt("| 命令行 | `%s` |\n", md_escape(rep.app.cmdline).c_str());
    b += fmt("| 用户 | uid=%d gid=%d |\n", rep.app.uid, rep.app.gid);
    b += fmt("| 首次连接 | %s |\n", fmt_time_full(rep.first_seen).c_str());
    b += fmt("| 报告生成 | %s |\n", fmt_time_full(rep.generated_at).c_str());

    b += "\n## 概览\n\n| 协议 | 连接数 | 消息总数 |\n|---|---|---|\n";
    const char* order[] = {"Wayland", "X11"};
    for (const char* p : order) {
        int n = 0, cnt = 0;
        for (auto& c : rep.connections)
            if (c.proto == p) {
                n++;
                cnt += c.msg_count;
            }
        if (n) b += fmt("| %s | %d | %d |\n", p, n, cnt);
    }

    for (auto& c : rep.connections) {
        b += fmt("\n---\n\n## 连接 #%d — %s（%s）\n\n", c.id, md_escape(c.proto).c_str(), md_escape(c.socket).c_str());
        b += fmt("- 开始: %s\n", fmt_time_full(c.start).c_str());
        if (c.closed)
            b += fmt("- 结束: %s\n", fmt_time_full(c.end).c_str());
        else
            b += "- 结束: (连接仍活跃或未正常关闭，报告生成时截止)\n";
        b += fmt("- 消息总数: %d\n", c.msg_count);
        if (c.truncated) b += fmt("- 注意: 消息日志超出上限，仅记录前 %d 条（统计数字仍为完整值）\n", c.logged);
        if (!c.meta.empty()) {
            b += "\n### 会话信息\n\n";
            for (auto& kv : c.meta) b += fmt("- %s: %s\n", kv.first.c_str(), md_escape(kv.second).c_str());
        }
        if (!c.stats.empty()) {
            if (c.proto == "Wayland")
                b += "\n### 使用的 Wayland 接口\n\n| 接口 | 客户端请求 | 服务端事件 |\n|---|---|---|\n";
            else
                b += "\n### X11 协议使用统计\n\n| 协议/扩展 | 客户端请求 | 服务端回复/事件 |\n|---|---|---|\n";
            for (auto& name : ifaces_sorted(c.stats)) {
                const IfaceStat& st = c.stats.at(name);
                b += fmt("| %s | %d | %d |\n", md_escape(name).c_str(), st.sent, st.recv);
            }
        }
        b += "\n### 消息日志\n\n";
        if (c.msgs.empty()) {
            b += "(无消息记录)\n";
            continue;
        }
        b += "| # | 时刻 | 方向 | 对象 | 消息 | 参数 | 字节 |\n|---|---|---|---|---|---|---|\n";
        int n = 0;
        for (auto& m : c.msgs) {
            std::string detail = m.detail;
            if (detail.size() > 300) detail = detail.substr(0, 300) + "…";
            b += fmt("| %d | %s | %s | %s | %s | %s | %d |\n", ++n, fmt_time_clock(m.time).c_str(),
                     md_escape(m.dir).c_str(), md_escape(m.object).c_str(), md_escape(m.name).c_str(),
                     md_escape(detail).c_str(), m.bytes);
        }
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << b;
    return bool(f);
}

std::string Tracker::write_report(const Report& rep) const {
    std::string base = fmt("%s_pid%d_%s", fmt_stamp(rep.first_seen).c_str(), rep.app.pid,
                           sanitize_name(rep.app.comm).c_str());
    std::vector<std::string> paths;
    if (format_ == "json" || format_ == "both") {
        std::string p = dir_ + "/" + base + ".json";
        if (write_json_report(p, rep)) paths.push_back(p);
    }
    if (format_ == "md" || format_ == "both") {
        std::string p = dir_ + "/" + base + ".md";
        if (write_markdown_report(p, rep)) paths.push_back(p);
    }
    if (paths.empty()) return "(报告写入失败)";
    return join_str(paths, " + ");
}
