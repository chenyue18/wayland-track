#include "proto_db.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include "logger.hpp"
#include "util.hpp"
#include "xml_min.hpp"

void ProtoDB::load_xml_string(const std::string& xml, const std::string& src_name) {
    XNode root = xml_parse(xml);
    if (root.name != "protocol" || root.children.empty()) {
        logf("跳过非 wayland 协议 XML: %s", src_name.c_str());
        return;
    }
    for (const XNode* in : root.children_of("interface")) {
        WIface iface;
        iface.name = in->attr("name");
        iface.version = atoi(in->attr("version", "0").c_str());
        if (iface.name.empty()) continue;
        for (const auto& m_up : in->children) {
        const XNode* m = m_up.get();
            bool is_req = m->name == "request";
            bool is_evt = m->name == "event";
            if (!is_req && !is_evt) continue;
            WMsgDef def;
            def.name = m->attr("name");
            for (const XNode* a : m->children_of("arg")) {
                WArg arg;
                arg.name = a->attr("name");
                arg.type = a->attr("type");
                arg.iface = a->attr("interface");
                def.args.push_back(std::move(arg));
            }
            if (is_req)
                iface.requests.push_back(std::move(def));
            else
                iface.events.push_back(std::move(def));
        }
        ifaces_[iface.name] = std::move(iface);
    }
}

void ProtoDB::load_embedded() {
    for (int i = 0; i < WT_EMBEDDED_PROTOCOL_XML_COUNT; i++) {
        load_xml_string(WT_EMBEDDED_PROTOCOL_XML[i], fmt("embedded:%d", i));
    }
}

static void walk_xml_files(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_xml_files(full, out);
        } else if (name.size() > 4 && name.substr(name.size() - 4) == ".xml") {
            out.push_back(full);
        }
    }
    closedir(d);
}

int ProtoDB::load_dirs(const std::vector<std::string>& dirs) {
    int count = 0;
    for (const std::string& dir : dirs) {
        std::vector<std::string> files;
        walk_xml_files(dir, files);
        for (auto& f : files) {
            std::string data;
            if (!read_file_str(f, data)) continue;
            size_t before = ifaces_.size();
            load_xml_string(data, f);
            if (ifaces_.size() > before) count++;
        }
    }
    return count;
}

const WMsgDef* ProtoDB::request(const std::string& iface, int opcode) const {
    auto it = ifaces_.find(iface);
    if (it == ifaces_.end() || opcode < 0 || size_t(opcode) >= it->second.requests.size()) return nullptr;
    return &it->second.requests[opcode];
}

const WMsgDef* ProtoDB::event(const std::string& iface, int opcode) const {
    auto it = ifaces_.find(iface);
    if (it == ifaces_.end() || opcode < 0 || size_t(opcode) >= it->second.events.size()) return nullptr;
    return &it->second.events[opcode];
}
