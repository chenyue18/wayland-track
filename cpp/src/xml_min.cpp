#include "xml_min.hpp"

#include <cstring>
#include <cctype>

#include "util.hpp"

namespace {

void decode_entities(std::string& s) {
    if (s.find('&') == std::string::npos) return;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") out += '&';
                else if (ent == "lt") out += '<';
                else if (ent == "gt") out += '>';
                else if (ent == "quot") out += '"';
                else if (ent == "apos") out += '\'';
                else if (!ent.empty() && ent[0] == '#' && ent.size() > 1) {
                    long cp = strtol(ent.c_str() + (ent[1] == 'x' || ent[1] == 'X' ? 2 : 1), nullptr,
                                     (ent[1] == 'x' || ent[1] == 'X') ? 16 : 10);
                    if (cp < 0x80) {
                        out.push_back(char(cp));
                    } else if (cp < 0x800) {
                        out.push_back(char(0xC0 | (cp >> 6)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(char(0xE0 | (cp >> 12)));
                        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    }
                } else {
                    out += s.substr(i, semi - i + 1);
                }
                i = semi + 1;
                continue;
            }
        }
        out.push_back(s[i]);
        i++;
    }
    s = out;
}

bool starts_with(const std::string& s, size_t pos, const char* pfx) {
    size_t n = strlen(pfx);
    return s.compare(pos, n, pfx) == 0;
}

void skip_until(std::string_view src, size_t& pos, const char* end_tag) {
    size_t e = src.find(end_tag, pos);
    pos = (e == std::string_view::npos) ? src.size() : e + strlen(end_tag);
}

}  // namespace

XNode xml_parse(const std::string& src) {
    XNode sentinel;
    std::vector<XNode*> stack{&sentinel};
    size_t i = 0;
    while (i < src.size()) {
        size_t lt = src.find('<', i);
        if (lt == std::string::npos) break;
        if (starts_with(src, lt, "<!--")) {
            skip_until(src, lt, "-->");
            i = lt;
            continue;
        }
        if (starts_with(src, lt, "<![CDATA[")) {
            size_t e = src.find("]]>", lt);
            i = (e == std::string::npos) ? src.size() : e + 3;
            continue;
        }
        if (starts_with(src, lt, "<?") || starts_with(src, lt, "<!")) {
            size_t e = src.find('>', lt);
            i = (e == std::string::npos) ? src.size() : e + 1;
            continue;
        }
        size_t gt = lt + 1;
        while (gt < src.size() && src[gt] != '>') {
            if (src[gt] == '"' || src[gt] == '\'') {
                char q = src[gt];
                size_t qe = src.find(q, gt + 1);
                if (qe == std::string::npos) {
                    gt = src.size();
                    break;
                }
                gt = qe + 1;
            } else {
                gt++;
            }
        }
        if (gt >= src.size()) break;
        std::string tag = src.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (!tag.empty() && tag[0] == '/') {
            if (stack.size() > 1) stack.pop_back();
            continue;
        }
        bool self_close = false;
        if (!tag.empty() && tag.back() == '/') {
            self_close = true;
            tag.pop_back();
        }
        size_t sp = tag.find_first_of(" \t\r\n");
        std::string name = (sp == std::string::npos) ? tag : tag.substr(0, sp);
        auto node = std::make_unique<XNode>();
        node->name = name;
        while (sp != std::string::npos) {
            size_t k = sp + 1;
            while (k < tag.size() && isspace((unsigned char)tag[k])) k++;
            if (k >= tag.size()) break;
            size_t eq = tag.find('=', k);
            if (eq == std::string::npos) break;
            std::string key = trim_str(tag.substr(k, eq - k));
            size_t q1 = tag.find_first_of("'\"", eq + 1);
            if (q1 == std::string::npos) break;
            char q = tag[q1];
            size_t q2 = tag.find(q, q1 + 1);
            if (q2 == std::string::npos) break;
            std::string val = tag.substr(q1 + 1, q2 - q1 - 1);
            decode_entities(val);
            node->attrs[key] = val;
            sp = q2;
        }
        XNode* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!self_close) stack.push_back(raw);
    }
    if (sentinel.children.empty()) return XNode{};
    return std::move(*sentinel.children[0]);
}
