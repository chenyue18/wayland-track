#include "x11.hpp"

#include <algorithm>
#include <cstring>

namespace {

int pad4(int n) { return (n + 3) & ~3; }

}  // namespace

X11Parser::X11Parser() = default;

uint16_t X11Parser::u16(const uint8_t* b, size_t off) const {
    return big_endian_ ? rd_be16(b + off) : rd_le16(b + off);
}

uint32_t X11Parser::u32(const uint8_t* b, size_t off) const {
    return big_endian_ ? rd_be32(b + off) : rd_le32(b + off);
}

FeedResult X11Parser::feed(int dir, const uint8_t* data, size_t len, int fd_count, TimePoint t) {
    std::lock_guard<std::mutex> lk(mu_);
    buf_[dir].insert(buf_[dir].end(), data, data + len);
    FeedResult res;
    const char* dir_name = (dir == x11::DIR_C2S) ? "C→S" : "S→C";

    while (!parse_err_[dir] && phase_[dir] != x11::PH_DEAD) {
        int consumed = 0;
        Msg m;
        bool ok;
        if (phase_[dir] == x11::PH_SETUP) {
            ok = step_setup(dir, dir_name, t, consumed, m);
        } else if (dir == x11::DIR_C2S) {
            ok = step_request(dir_name, fd_count, t, consumed, m);
        } else {
            ok = step_reply(dir_name, t, consumed, m);
        }
        if (!ok) break;
        if (consumed > 0) {
            res.bytes += consumed;
            if (!m.name.empty()) res.msgs.push_back(std::move(m));
        }
    }
    if (buf_[dir].empty()) buf_[dir].shrink_to_fit();
    return res;
}

bool X11Parser::step_setup(int dir, const char* dir_name, TimePoint t, int& consumed, Msg& m) {
    auto& buf = buf_[dir];
    if (dir == x11::DIR_C2S) {
        if (buf.size() < 12) return false;
        uint8_t order = buf[0];
        if (order == 'l')
            big_endian_ = false;
        else if (order == 'B')
            big_endian_ = true;
        else {
            parse_err_[dir] = true;
            return false;
        }
        int name_len = u16(buf.data(), 6);
        int data_len = u16(buf.data(), 8);
        int need = 12 + pad4(name_len) + pad4(data_len);
        if (int(buf.size()) < need) return false;
        std::string auth_name;
        if (name_len > 0) auth_name.assign(reinterpret_cast<const char*>(buf.data()) + 12, size_t(name_len));
        phase_[dir] = x11::PH_STREAM;
        seq_ = 1;
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = "Setup(握手)";
        m.object = fmt("byte-order=%c", order);
        m.detail = fmt("认证方式=\"%s\"", auth_name.c_str());
        m.bytes = need;
        buf.erase(buf.begin(), buf.begin() + need);
        consumed = need;
        return true;
    }

    if (buf.size() < 8) return false;
    uint8_t success = buf[0];
    int add_len = u16(buf.data(), 6);
    int total = 8 + add_len * 4;
    if (success == 0) {
        if (int(buf.size()) < total) return false;
        std::string reason;
        if (buf.size() >= 2) {
            int rl = buf[1];
            if (32 + rl <= int(buf.size())) reason.assign(reinterpret_cast<const char*>(buf.data()) + 32, size_t(rl));
        }
        phase_[dir] = x11::PH_DEAD;
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = "Setup-Fail(连接被拒)";
        m.detail = "原因=" + reason;
        m.bytes = total;
        buf.erase(buf.begin(), buf.begin() + total);
        consumed = total;
        return true;
    }
    if (success == 2) {
        if (int(buf.size()) < total) return false;
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = "Setup-RequireAuth";
        m.bytes = total;
        buf.erase(buf.begin(), buf.begin() + total);
        consumed = total;
        return true;
    }
    if (success == 1) {
        if (int(buf.size()) < total) return false;
        major_ = u16(buf.data(), 2);
        minor_ = u16(buf.data(), 4);
        release_ = u32(buf.data(), 8);
        int vlen = u16(buf.data(), 24);
        if (40 + vlen <= int(buf.size())) {
            vendor_.assign(reinterpret_cast<const char*>(buf.data()) + 40, size_t(vlen));
            while (!vendor_.empty() && vendor_.back() == '\0') vendor_.pop_back();
        }
        phase_[dir] = x11::PH_STREAM;
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = "Setup-Success";
        m.detail = fmt("vendor=\"%s\" 协议=%d.%d release=%u", vendor_.c_str(), major_, minor_, release_);
        m.bytes = total;
        buf.erase(buf.begin(), buf.begin() + total);
        consumed = total;
        return true;
    }
    parse_err_[dir] = true;
    return false;
}

std::string X11Parser::atom_name(uint32_t id) const {
    auto it = atoms_.find(id);
    if (it != atoms_.end()) return fmt("%s(0x%x)", it->second.c_str(), id);
    return fmt("0x%x", id);
}

std::string X11Parser::ext_event_name(uint8_t code) const {
    const char* best = nullptr;
    uint8_t best_first = 0;
    for (auto& e : ext_events_) {
        if (code >= e.first && (!best || e.first >= best_first)) {
            best = e.name.c_str();
            best_first = e.first;
        }
    }
    return best ? std::string(best) : std::string();
}

std::string X11Parser::decode_request(int op, const uint8_t* b, size_t len) const {
    switch (op) {
    case 1: {
        if (len < 8) break;
        uint32_t w = u32(b, 4);
        uint32_t par = u32(b, 8);
        return fmt("win=0x%x parent=0x%x depth=%d", w, par, len > 1 ? b[1] : 0);
    }
    case 2: case 3: case 4: case 5: case 8: case 9: case 10: case 11:
    case 14: case 15: case 17: case 21: case 25: case 39: {
        if (len < 8) break;
        return fmt("win=0x%x", u32(b, 4));
    }
    case 12: {
        if (len < 10) break;
        uint32_t mask = u16(b, 8);
        return fmt("win=0x%x mask=0x%04x", u32(b, 4), mask);
    }
    case 16: {
        if (len < 6) break;
        uint32_t nl = u16(b, 4);
        if (6 + nl > len) break;
        return fmt("name=\"%.*s\" only-if-exists=%d", int(nl), reinterpret_cast<const char*>(b) + 6, b[1]);
    }
    case 18: {
        if (len < 24 || len <= 16) break;
        uint32_t w = u32(b, 4);
        uint32_t prop = u32(b, 8);
        uint32_t typ = u32(b, 12);
        uint32_t n = u32(b, 20);
        return fmt("win=0x%x prop=%s type=%s fmt=%d n=%u", w, atom_name(prop).c_str(), atom_name(typ).c_str(), b[16], n);
    }
    case 19: case 20: {
        if (len < 12) break;
        return fmt("win=0x%x prop=%s", u32(b, 4), atom_name(u32(b, 8)).c_str());
    }
    case 22: {
        if (len < 12) break;
        return fmt("owner=0x%x selection=%s", u32(b, 4), atom_name(u32(b, 8)).c_str());
    }
    case 24: {
        if (len < 20) break;
        return fmt("requestor=0x%x selection=%s target=%s property=%s", u32(b, 4), atom_name(u32(b, 8)).c_str(),
                   atom_name(u32(b, 12)).c_str(), atom_name(u32(b, 16)).c_str());
    }
    case 45: {
        if (len < 10) break;
        uint32_t fid = u32(b, 4);
        uint32_t nl = u16(b, 8);
        std::string name;
        if (10 + nl <= len) name.assign(reinterpret_cast<const char*>(b) + 10, nl);
        return fmt("font=0x%x name=\"%s\"", fid, name.c_str());
    }
    case 53: {
        if (len < 12) break;
        return fmt("pixmap=0x%x drawable=0x%x", u32(b, 4), u32(b, 8));
    }
    case 54: case 60: case 95: {
        if (len < 8) break;
        return fmt("id=0x%x", u32(b, 4));
    }
    case 55: case 56: {
        if (len < 12) break;
        return fmt("gc=0x%x drawable=0x%x", u32(b, 4), u32(b, 8));
    }
    case 47: {
        if (len < 8) break;
        return fmt("drawable=0x%x", u32(b, 4));
    }
    case 72: case 73: {
        if (len < 20) break;
        uint32_t w = u16(b, 16);
        uint32_t h = u16(b, 18);
        return fmt("drawable=0x%x gc=0x%x %ux%u", u32(b, 4), u32(b, 8), w, h);
    }
    case 74: case 75: case 76: case 77: {
        if (len < 12) break;
        return fmt("win=0x%x gc=0x%x", u32(b, 4), u32(b, 8));
    }
    case 93: {
        if (len < 8) break;
        return fmt("cursor=0x%x", u32(b, 4));
    }
    case 98: {
        if (len < 6) break;
        uint32_t nl = u16(b, 4);
        if (8 + nl > len) break;
        return fmt("name=\"%.*s\"", int(nl), reinterpret_cast<const char*>(b) + 8);
    }
    case 127:
        return "空操作";
    }
    return fmt("%zu 字节", len);
}

bool X11Parser::step_request(const char* dir_name, int fd_count, TimePoint t, int& consumed, Msg& m) {
    auto& buf = buf_[x11::DIR_C2S];
    if (buf.size() < 4) return false;
    uint8_t opcode = buf[0];
    uint8_t real_opcode = opcode;
    int total = 0;
    if (opcode == 254) {
        if (buf.size() < 8) return false;
        real_opcode = buf[1];
        total = int(u32(buf.data(), 4)) * 4;
        if (total < 8) {
            parse_err_[x11::DIR_C2S] = true;
            return false;
        }
        if (total > int(buf.size())) return false;
    } else {
        total = int(u16(buf.data(), 2)) * 4;
        if (total < 4) {
            parse_err_[x11::DIR_C2S] = true;
            return false;
        }
        if (total > int(buf.size())) return false;
    }
    if (real_opcode == 0) {
        parse_err_[x11::DIR_C2S] = true;
        return false;
    }

    std::string iface = "core";
    std::string name = fmt("Request-%d", real_opcode);
    std::string detail = fmt("%d 字节", total);
    if (real_opcode >= 128) {
        auto it = ext_name_.find(real_opcode);
        if (it != ext_name_.end()) {
            iface = it->second;
            name = fmt("%s.minor-%d", it->second.c_str(), buf[1]);
        } else {
            iface = fmt("ext#%d", real_opcode);
            name = fmt("ext#%d(minor=%d)", real_opcode, buf[1]);
        }
    } else {
        const char* rn = x11_req_name(int(real_opcode));
        if (rn) name = rn;
        detail = decode_request(int(real_opcode), buf.data(), size_t(total));
    }
    if (fd_count > 0) detail += fmt(" (+%d fd)", fd_count);

    uint16_t seq = seq_++;
    if (real_opcode == 98) {
        uint32_t nl = u16(buf.data(), 4);
        if (8 + nl <= size_t(total)) pending_[seq] = std::string(reinterpret_cast<const char*>(buf.data()) + 8, nl);
    } else if (real_opcode == 16) {
        uint32_t nl = u16(buf.data(), 4);
        if (6 + nl <= size_t(total)) pending_[seq] = "atom:" + std::string(reinterpret_cast<const char*>(buf.data()) + 6, nl);
    }

    m.time = t;
    m.dir = dir_name;
    m.iface = iface;
    m.name = name;
    m.detail = detail;
    m.bytes = total;
    buf.erase(buf.begin(), buf.begin() + total);
    consumed = total;
    return true;
}

bool X11Parser::step_reply(const char* dir_name, TimePoint t, int& consumed, Msg& m) {
    auto& buf = buf_[x11::DIR_S2C];
    if (buf.empty()) return false;
    uint8_t first = buf[0];
    const uint8_t* b = buf.data();
    if (first == 0) {
        if (buf.size() < 32) return false;
        uint8_t code = b[1];
        uint16_t seq = u16(b, 2);
        std::string name = fmt("Error-%d", code);
        if (const char* n = x11_err_name(code)) name = std::string("Error:") + n;
        auto it = pending_.find(seq);
        if (it != pending_.end()) {
            name += fmt(" [%s]", it->second.c_str());
            pending_.erase(it);
        }
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = name;
        m.detail = fmt("seq=%d", seq);
        m.bytes = 32;
        buf.erase(buf.begin(), buf.begin() + 32);
        consumed = 32;
        return true;
    }
    if (first == 1) {
        if (buf.size() < 8) return false;
        uint32_t add_len = u32(b, 4);
        int total = 32 + int(add_len) * 4;
        if (add_len > 0x100000) {
            parse_err_[x11::DIR_S2C] = true;
            return false;
        }
        if (total > int(buf.size())) return false;
        uint16_t seq = u16(b, 2);
        std::string name = "Reply";
        std::string detail = fmt("seq=%d %d 字节", seq, total);
        auto it = pending_.find(seq);
        if (it != pending_.end()) {
            std::string pending = it->second;
            pending_.erase(it);
            if (pending.rfind("atom:", 0) == 0) {
                std::string atom_name_str = pending.substr(5);
                if (8 + 4 <= size_t(total)) {
                    uint32_t aid = u32(b, 8);
                    atoms_[aid] = atom_name_str;
                    detail = fmt("seq=%d atom \"%s\" = 0x%x", seq, atom_name_str.c_str(), aid);
                    name = "InternAtom-Reply";
                }
            } else {
                uint8_t present = b[8];
                if (present == 1) {
                    uint8_t major = b[9];
                    uint8_t first_evt = b[10];
                    ext_name_[major] = pending;
                    ext_events_.push_back({first_evt, pending});
                    ext_list_.push_back(fmt("%s(op=%d,evt=%d)", pending.c_str(), major, first_evt));
                    name = "QueryExtension-Reply";
                    detail = fmt("seq=%d 扩展=\"%s\" major-opcode=%d first-event=%d", seq, pending.c_str(), major,
                                 first_evt);
                } else {
                    name = "QueryExtension-Reply(不存在)";
                    detail = fmt("seq=%d 扩展=\"%s\" 不存在", seq, pending.c_str());
                }
            }
        }
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = name;
        m.detail = detail;
        m.bytes = total;
        buf.erase(buf.begin(), buf.begin() + total);
        consumed = total;
        return true;
    }
    if (first == 35) {
        if (buf.size() < 8) return false;
        uint32_t add_len = u32(b, 4);
        int total = 32 + int(add_len) * 4;
        if (add_len > 0x100000) {
            parse_err_[x11::DIR_S2C] = true;
            return false;
        }
        if (total > int(buf.size())) return false;
        uint8_t ext = b[1];
        uint16_t evtype = u16(b, 8);
        std::string extn = fmt("ext#%d", ext);
        auto it = ext_name_.find(ext);
        if (it != ext_name_.end()) extn = it->second;
        m.time = t;
        m.dir = dir_name;
        m.iface = extn;
        m.name = fmt("GenericEvent(type=%d)", evtype);
        m.bytes = total;
        buf.erase(buf.begin(), buf.begin() + total);
        consumed = total;
        return true;
    }
    if (first >= 2 && first <= 34) {
        if (buf.size() < 32) return false;
        std::string name = fmt("Event-%d", first);
        if (const char* n = x11_evt_name(first)) name = n;
        std::string detail;
        if (first != 11) detail = fmt("seq=%d", u16(b, 2));
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = name;
        m.detail = detail;
        m.bytes = 32;
        buf.erase(buf.begin(), buf.begin() + 32);
        consumed = 32;
        return true;
    }
    if (first >= 64) {
        if (buf.size() < 32) return false;
        std::string name = fmt("ExtEvent-%d", first);
        std::string extn = ext_event_name(first);
        if (!extn.empty()) name = extn + "-Event";
        m.time = t;
        m.dir = dir_name;
        m.iface = "x11";
        m.name = name;
        m.detail = fmt("seq=%d", u16(b, 2));
        m.bytes = 32;
        buf.erase(buf.begin(), buf.begin() + 32);
        consumed = 32;
        return true;
    }
    parse_err_[x11::DIR_S2C] = true;
    return false;
}

std::map<std::string, std::string> X11Parser::summary() {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, std::string> m;
    if (!vendor_.empty()) {
        m["server_vendor"] = vendor_;
        m["protocol_version"] = fmt("%d.%d", major_, minor_);
        m["server_release"] = fmt("%u", release_);
    }
    if (!ext_list_.empty()) {
        std::sort(ext_list_.begin(), ext_list_.end());
        m["x11_extensions"] = join_str(ext_list_, ", ");
    }
    if (!atoms_.empty()) m["known_atoms"] = fmt("%zu 个（通过 InternAtom 记录）", atoms_.size());
    for (int d = 0; d < 2; d++) {
        if (parse_err_[d]) {
            m["parse_error"] = "X11 流解析失败，出错位置之后的数据未解码（转发不受影响）";
            break;
        }
    }
    return m;
}
