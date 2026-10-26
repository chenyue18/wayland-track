#pragma once

#include <map>
#include <string>
#include <vector>

struct WArg {
    std::string name;
    std::string type;
    std::string iface;
};

struct WMsgDef {
    std::string name;
    std::vector<WArg> args;
};

struct WIface {
    std::string name;
    int version = 0;
    std::vector<WMsgDef> requests;
    std::vector<WMsgDef> events;
};

class ProtoDB {
public:
    void load_xml_string(const std::string& xml, const std::string& src_name);
    void load_embedded();
    int load_dirs(const std::vector<std::string>& dirs);

    const WMsgDef* request(const std::string& iface, int opcode) const;
    const WMsgDef* event(const std::string& iface, int opcode) const;
    size_t iface_count() const { return ifaces_.size(); }
    const WIface* iface(const std::string& name) const {
        auto it = ifaces_.find(name);
        return it == ifaces_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, WIface> ifaces_;
};

extern const char* const WT_EMBEDDED_PROTOCOL_XML[];
extern const int WT_EMBEDDED_PROTOCOL_XML_COUNT;
