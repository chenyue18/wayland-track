#!/usr/bin/env python3
"""从 xproto.xml 生成 X11 请求 opcode 表 → src/gen/x11_tables.cpp"""
import os
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
XPROTO = os.path.join(HERE, "xproto.xml")
OUT = os.path.join(HERE, "..", "src", "gen", "x11_tables.cpp")


def table(name, count_name, entries):
    lines = [f"const WTXNameEntry {name}[] = {{"]
    for op, nm in entries:
        lines.append(f'    {{{op}, "{nm}"}},')
    lines.append("};")
    lines.append(f"const int {count_name} = {len(entries)};")
    return "\n".join(lines)


def main():
    root = ET.parse(XPROTO).getroot()
    reqs = [(int(r.get("opcode")), r.get("name")) for r in root.findall("request")]
    header = """// 由 tools/gen_x11.py 生成，请勿手改
#include "../x11_tables.hpp"
"""
    body = "\n\n".join([
        table("WT_X11_REQ_NAMES", "WT_X11_REQ_NAME_COUNT", reqs),
    ])
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        f.write(header + "\n" + body + "\n")
    print(f"generated {OUT}: {len(reqs)} requests")


if __name__ == "__main__":
    sys.exit(main())
