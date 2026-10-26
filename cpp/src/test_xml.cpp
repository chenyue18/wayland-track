#include <cassert>

#include "test_util.hpp"
#include "util.hpp"
#include "xml_min.hpp"

void run_test_xml() {
    XNode root = xml_parse(R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- comment <with> tags -->
<protocol name="wayland">
  <copyright>Copyright &amp; left &lt;tag&gt;</copyright>
  <interface name="wl_display" version="1">
    <request name="sync">
      <arg name="callback" type="new_id" interface="wl_callback"/>
    </request>
    <event name="error">
      <arg name="object_id" type="object"/>
      <arg name="code" type="uint" enum="error"/>
    </event>
  </interface>
</protocol>)");
    CHECK(root.name == "protocol");
    CHECK(root.attr("name") == "wayland");
    CHECK(root.children.size() == 2);
    CHECK(root.children[0]->name == "copyright");
    CHECK(root.children[0]->name == "copyright");
    const XNode* iface = root.child("interface");
    CHECK(iface != nullptr);
    CHECK(iface->attr("name") == "wl_display");
    CHECK(iface->children.size() == 2);
    const XNode* req = iface->child("request");
    CHECK(req != nullptr);
    CHECK(req->attr("name") == "sync");
    const XNode* arg = req->child("arg");
    CHECK(arg != nullptr);
    CHECK(arg->attr("interface") == "wl_callback");
    const XNode* evt = iface->child("event");
    CHECK(evt != nullptr);
    CHECK(evt->children.size() == 2);
    CHECK(evt->children[1]->attr("enum") == "error");
}
