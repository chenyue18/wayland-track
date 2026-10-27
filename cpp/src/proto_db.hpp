#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct WArg {
    std::string name;
    std::string type;
    std::string iface;
    std::string enum_name;
};

struct WMsgDef {
    std::string name;
    std::vector<WArg> args;
};

struct WEnumEntry {
    std::string name;
    long long value = 0;
};

struct WEnum {
    std::string name;
    bool bitfield = false;
    std::vector<WEnumEntry> entries;
};

struct WIface {
    std::string name;
    int version = 0;
    std::vector<WMsgDef> requests;
    std::vector<WMsgDef> events;
    std::vector<WEnum> enums;
};

class ProtoDB {
public:
    void load_xml_string(const std::string& xml, const std::string& src_name);
    void load_embedded();
    int load_dirs(const std::vector<std::string>& dirs);

    const WMsgDef* request(const std::string& iface, int opcode) const;
    const WMsgDef* event(const std::string& iface, int opcode) const;
    const WEnum* enum_of(const std::string& iface, const std::string& enum_name) const;
    static std::string format_enum_value(const WEnum* e, uint32_t v);
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
