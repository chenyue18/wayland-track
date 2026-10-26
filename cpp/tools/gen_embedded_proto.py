#!/usr/bin/env python3
"""将 proto/*.xml 嵌入为 C++ 字符串字面量，生成 src/gen/embedded_proto.cpp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_DIR = os.path.join(HERE, "..", "proto")
OUT = os.path.join(HERE, "..", "src", "gen", "embedded_proto.cpp")


def raw_literal(idx: int, content: str) -> str:
    d = 0
    while True:
        delim = f"WTXML{idx}" if d == 0 else f"WTXML{idx}_{d}"
        if f"){delim}" not in content:
            break
        d += 1
    return f'R"{delim}({content}){delim}"'


def main():
    names = sorted(f for f in os.listdir(PROTO_DIR) if f.endswith(".xml"))
    parts = [
        "// 由 tools/gen_embedded_proto.py 生成，请勿手改",
        "",
        "#include <cstddef>",
        "",
        "extern const char* const WT_EMBEDDED_PROTOCOL_XML[];",
        "extern const int WT_EMBEDDED_PROTOCOL_XML_COUNT;",
        "",
        "const char* const WT_EMBEDDED_PROTOCOL_XML[] = {",
    ]
    for i, name in enumerate(names):
        with open(os.path.join(PROTO_DIR, name), "r", encoding="utf-8") as f:
            content = f.read()
        parts.append(f"    // {name}")
        parts.append(f"    {raw_literal(i, content)},")
    parts.append("};")
    parts.append(f"const int WT_EMBEDDED_PROTOCOL_XML_COUNT = {len(names)};")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(parts) + "\n")
    print(f"generated {OUT}: {len(names)} files")


if __name__ == "__main__":
    sys.exit(main())
