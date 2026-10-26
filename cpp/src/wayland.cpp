#include "wayland.hpp"

#include <algorithm>
#include <cstring>

namespace {

std::string format_fixed(int32_t f) {
    double v = double(f) / 256.0;
    if (v == int64_t(v)) return fmt("%lld", (long long)v);
    return fmt("%.4g", v);
}

std::string raw_words(const uint8_t* p, size_t n) {
    std::string s = "raw: ";
    char buf[16];
    size_t i = 0;
    for (; i + 4 <= n && i < 64; i += 4) {
        snprintf(buf, sizeof buf, "%08x", rd_le32(p + i));
        if (i) s += ' ';
        s += buf;
    }
    if (n > 64) s += " ...";
    if (s == "raw: ") return fmt("<%zu 字节 raw>", n);
    return s;
}

}  // namespace

WaylandParser::WaylandParser(std::shared_ptr<const ProtoDB> db) : db_(std::move(db)) {
    objects_[1] = "wl_display";
}

std::string WaylandParser::obj_name(uint32_t id) const {
    auto it = objects_.find(id);
    if (it != objects_.end()) return fmt("%s@%u", it->second.c_str(), id);
    if (id >= 0xff000000u) return fmt("server-object@%u", id);
    return fmt("object@%u", id);
}

FeedResult WaylandParser::feed(int dir, const uint8_t* data, size_t len, int fd_count, TimePoint t) {
    std::lock_guard<std::mutex> lk(mu_);
    fd_avail_[dir] += fd_count;
    buf_[dir].insert(buf_[dir].end(), data, data + len);

    FeedResult res;
    const char* dir_name = (dir == wayland::DIR_C2S) ? "C→S" : "S→C";

    while (!parse_err_[dir]) {
        const size_t have = buf_[dir].size();
        if (have < 8) break;
        const uint8_t* b = buf_[dir].data();
        uint32_t obj_id = rd_le32(b);
        uint16_t opcode = rd_le16(b + 4);
        int size = int(rd_le16(b + 6));
        if (size < 8) {
            parse_err_[dir] = true;
            break;
        }
        if (size > int(have)) break;
        const uint8_t* payload = b + 8;
        size_t plen = size_t(size - 8);
        std::string iface;
        auto oit = objects_.find(obj_id);
        if (oit != objects_.end()) iface = oit->second;
        Decoded d = decode(dir, iface, obj_id, int(opcode), payload, plen);
        Msg m;
        m.time = t;
        m.dir = dir_name;
        m.iface = iface.empty() ? "unknown" : iface;
        m.name = d.name;
        m.object = obj_name(obj_id);
        m.detail = d.detail;
        m.bytes = size;
        res.msgs.push_back(std::move(m));
        res.bytes += size;
        buf_[dir].erase(buf_[dir].begin(), buf_[dir].begin() + size);
        if (objects_.size() > 100000) {
            objects_.clear();
            objects_[1] = "wl_display";
        }
    }
    if (buf_[dir].empty()) buf_[dir].shrink_to_fit();
    return res;
}

std::string WaylandParser::decode_arg(int dir, ArgReader& rd, const WArg& a, const std::string& iface) {
    const char* name = a.name.c_str();
    if (a.type == "int") {
        int32_t v;
        if (!rd.i32(v)) return {};
        return fmt("%s=%d", name, v);
    }
    if (a.type == "uint") {
        uint32_t v;
        if (!rd.u32(v)) return {};
        return fmt("%s=%u", name, v);
    }
    if (a.type == "fixed") {
        int32_t v;
        if (!rd.i32(v)) return {};
        return fmt("%s=%s", name, format_fixed(v).c_str());
    }
    if (a.type == "string") {
        std::string v;
        if (!rd.str(v)) return {};
        return fmt("%s=\"%s\"", name, v.c_str());
    }
    if (a.type == "object") {
        uint32_t v;
        if (!rd.u32(v)) return {};
        return fmt("%s=%s", name, obj_name(v).c_str());
    }
    if (a.type == "new_id") {
        if (a.iface.empty()) {
            std::string sname;
            uint32_t ver, id;
            if (!rd.str(sname)) return {};
            if (!rd.u32(ver)) return {};
            if (!rd.u32(id)) return {};
            objects_[id] = sname;
            if (iface == "wl_registry" && dir == wayland::DIR_C2S)
                binds_.push_back(fmt("%s@%u (v%u)", sname.c_str(), id, ver));
            return fmt("%s=%s@%u v%u", name, sname.c_str(), id, ver);
        }
        uint32_t id;
        if (!rd.u32(id)) return {};
        objects_[id] = a.iface;
        return fmt("%s=%s@%u", name, a.iface.c_str(), id);
    }
    if (a.type == "array") {
        uint32_t n;
        if (!rd.u32(n)) return {};
        if (!rd.skip_bytes(n)) return {};
        return fmt("%s=<%u bytes>", name, n);
    }
    if (a.type == "fd") {
        if (fd_avail_[dir] > 0) {
            fd_avail_[dir]--;
            int idx = fd_total_[dir]++;
            return fmt("%s=fd#%d", name, idx);
        }
        fd_debt_[dir]++;
        return fmt("%s=fd(?)", name);
    }
    uint32_t v;
    if (!rd.u32(v)) return {};
    return fmt("%s=%u", name, v);
}

Decoded WaylandParser::decode(int dir, const std::string& iface, uint32_t obj_id, int opcode, const uint8_t* payload,
                              size_t plen) {
    (void)obj_id;
    const WMsgDef* def = nullptr;
    if (!iface.empty()) {
        def = (dir == wayland::DIR_C2S) ? db_->request(iface, opcode) : db_->event(iface, opcode);
    }
    if (!def) {
        Decoded d;
        d.name = fmt("opcode-%d", opcode);
        d.detail = raw_words(payload, plen);
        return d;
    }

    ArgReader rd(payload, plen);
    Decoded d;
    d.name = def->name;
    bool first = true;
    for (const WArg& a : def->args) {
        std::string v = decode_arg(dir, rd, a, iface);
        if (v.empty()) {
            d.detail += fmt("%s=<malformed>", a.name.c_str());
            break;
        }
        if (!first) d.detail += ", ";
        first = false;
        d.detail += v;
    }

    if (d.name == "delete_id" && dir == wayland::DIR_S2C && iface == "wl_display" && plen >= 4) {
        objects_.erase(rd_le32(payload));
    }
    if (d.name == "global" && iface == "wl_registry" && dir == wayland::DIR_S2C) {
        ArgReader r2(payload, plen);
        uint32_t n;
        std::string s;
        if (r2.u32(n) && r2.str(s)) globals_[n] = s;
    }
    return d;
}

std::map<std::string, std::string> WaylandParser::summary() {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, std::string> m;
    if (!globals_.empty()) {
        std::vector<std::string> names;
        for (auto& kv : globals_) names.push_back(kv.second);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        m["registry_globals"] = join_str(names, ", ");
    }
    if (!binds_.empty()) {
        std::map<std::string, int> counts;
        std::vector<std::string> order;
        for (auto& b : binds_) {
            std::string iface = b;
            size_t at = iface.find('@');
            if (at != std::string::npos && at > 0) iface = iface.substr(0, at);
            if (!counts.count(iface)) order.push_back(iface);
            counts[iface]++;
        }
        std::vector<std::string> parts;
        for (auto& i : order) parts.push_back(fmt("%s×%d", i.c_str(), counts[i]));
        std::string joined = join_str(parts, ", ");
        if (parts.size() > 40) {
            parts.resize(40);
            m["bound_objects"] = fmt("%zu 个不同接口: %s ...", counts.size(), join_str(parts, ", ").c_str());
        } else {
            m["bound_objects"] = fmt("%zu 个不同接口: %s", counts.size(), joined.c_str());
        }
    }
    for (int d = 0; d < 2; d++) {
        if (parse_err_[d]) {
            m["parse_error"] = "wire 流解析失败，出错位置之后的数据未解码（转发不受影响）";
            break;
        }
    }
    return m;
}
