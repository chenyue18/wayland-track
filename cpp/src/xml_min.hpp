#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

struct XNode {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::vector<std::unique_ptr<XNode>> children;

    const XNode* child(const std::string& n) const {
        for (auto& c : children)
            if (c->name == n) return c.get();
        return nullptr;
    }
    std::vector<const XNode*> children_of(const std::string& n) const {
        std::vector<const XNode*> out;
        for (auto& c : children)
            if (c->name == n) out.push_back(c.get());
        return out;
    }
    std::string attr(const std::string& n, const std::string& dflt = "") const {
        auto it = attrs.find(n);
        return it == attrs.end() ? dflt : it->second;
    }
};

XNode xml_parse(const std::string& src);
