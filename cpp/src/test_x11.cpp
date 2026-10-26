#include <cstring>

#include "test_util.hpp"
#include "util.hpp"
#include "x11.hpp"

namespace {

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((x >> 8) & 0xff);
    v.push_back(x & 0xff);
}

void put_u16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
}

void put_u32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 24) & 0xff);
}

std::vector<uint8_t> xreq(uint8_t op, uint8_t minor, const std::vector<uint8_t>& payload) {
    size_t padded = (4 + payload.size() + 3) & ~size_t(3);
    std::vector<uint8_t> v;
    v.push_back(op);
    v.push_back(minor);
    put_u16le(v, uint16_t(padded / 4));
    v.insert(v.end(), payload.begin(), payload.end());
    while (v.size() < padded) v.push_back(0);
    return v;
}

std::vector<uint8_t> csetup() {
    std::vector<uint8_t> v;
    v.push_back('l');
    v.push_back(0);
    for (int i = 0; i < 5; i++) put_u16le(v, 0);
    return v;
}

}  // namespace

void x11_chunked_and_big();

void run_test_x11() {
    X11Parser p;
    TimePoint now = Clock::now();

    auto cs = csetup();
    auto r1 = p.feed(x11::DIR_C2S, cs.data(), cs.size(), 0, now);
    CHECK(r1.msgs.size() == 1);
    CHECK(r1.msgs[0].name == "Setup(握手)");
    CHECK_CONTAIN(r1.msgs[0].detail, "认证方式=\"\"");

    std::vector<uint8_t> srep;
    srep.push_back(1);
    srep.push_back(0);
    put_u16le(srep, 11);
    put_u16le(srep, 0);
    put_u16le(srep, 10);
    put_u32le(srep, 12345);
    put_u32le(srep, 0x00400000);
    put_u32le(srep, 0x001fffff);
    put_u32le(srep, 256);
    put_u16le(srep, 5);
    put_u16le(srep, 65535);
    for (int i = 0; i < 8; i++) srep.push_back(0);
    for (int i = 0; i < 4; i++) srep.push_back(0);
    std::string vendor = "Test";
    vendor.append(4, '\0');
    srep.insert(srep.end(), vendor.begin(), vendor.end());
    auto r2 = p.feed(x11::DIR_S2C, srep.data(), srep.size(), 0, now);
    CHECK(r2.msgs.size() == 1);
    CHECK(r2.msgs[0].name == "Setup-Success");
    CHECK_CONTAIN(r2.msgs[0].detail, "vendor=\"Test\"");

    std::vector<uint8_t> cw;
    put_u32le(cw, 0x1c00001);
    put_u32le(cw, 0x1c00000);
    put_u32le(cw, 0);
    auto r3bytes = xreq(1, 24, cw);
    auto r3 = p.feed(x11::DIR_C2S, r3bytes.data(), r3bytes.size(), 0, now);
    CHECK(r3.msgs.size() == 1);
    CHECK(r3.msgs[0].name == "CreateWindow");
    CHECK_CONTAIN(r3.msgs[0].detail, "win=0x1c00001");
    CHECK_CONTAIN(r3.msgs[0].detail, "depth=24");

    std::vector<uint8_t> ia;
    put_u16le(ia, 5);
    ia.insert(ia.end(), {'H', 'E', 'L', 'L', 'O', 0, 0, 0});
    auto r4bytes = xreq(16, 1, ia);
    auto r4 = p.feed(x11::DIR_C2S, r4bytes.data(), r4bytes.size(), 0, now);
    CHECK(r4.msgs.size() == 1);
    CHECK(r4.msgs[0].name == "InternAtom");

    std::vector<uint8_t> reply;
    reply.push_back(1);
    reply.push_back(0);
    put_u16le(reply, 2);
    put_u32le(reply, 0);
    put_u32le(reply, 42);
    for (int i = 0; i < 20; i++) reply.push_back(0);
    auto r5 = p.feed(x11::DIR_S2C, reply.data(), reply.size(), 0, now);
    CHECK(r5.msgs.size() == 1);
    CHECK(r5.msgs[0].name == "InternAtom-Reply");

    std::vector<uint8_t> qe;
    put_u16le(qe, 5);
    put_u16le(qe, 0);
    qe.insert(qe.end(), {'R', 'A', 'N', 'D', 'R', 0, 0, 0});
    auto r6bytes = xreq(98, 0, qe);
    auto r6 = p.feed(x11::DIR_C2S, r6bytes.data(), r6bytes.size(), 0, now);
    CHECK(r6.msgs.size() == 1);
    CHECK_CONTAIN(r6.msgs[0].detail, "name=\"RANDR\"");

    std::vector<uint8_t> qreply;
    qreply.push_back(1);
    qreply.push_back(0);
    put_u16le(qreply, 3);
    put_u32le(qreply, 0);
    qreply.push_back(1);
    qreply.push_back(140);
    qreply.push_back(89);
    qreply.push_back(90);
    for (int i = 0; i < 20; i++) qreply.push_back(0);
    auto r7 = p.feed(x11::DIR_S2C, qreply.data(), qreply.size(), 0, now);
    CHECK(r7.msgs.size() == 1);
    CHECK(r7.msgs[0].name == "QueryExtension-Reply");

    auto r8bytes = xreq(140, 5, std::vector<uint8_t>(4, 0));
    auto r8 = p.feed(x11::DIR_C2S, r8bytes.data(), r8bytes.size(), 0, now);
    CHECK(r8.msgs.size() == 1);
    CHECK(r8.msgs[0].iface == "RANDR");

    std::vector<uint8_t> expose;
    expose.push_back(12);
    expose.push_back(0);
    put_u16le(expose, 4);
    put_u32le(expose, 0x1c00001);
    for (int i = 0; i < 24; i++) expose.push_back(0);
    auto r9 = p.feed(x11::DIR_S2C, expose.data(), expose.size(), 0, now);
    CHECK(r9.msgs.size() == 1);
    CHECK(r9.msgs[0].name == "Expose");

    std::vector<uint8_t> ge;
    ge.push_back(35);
    ge.push_back(140);
    put_u16le(ge, 5);
    put_u32le(ge, 0);
    put_u16le(ge, 1);
    put_u16le(ge, 0);
    for (int i = 0; i < 20; i++) ge.push_back(0);
    auto r10 = p.feed(x11::DIR_S2C, ge.data(), ge.size(), 0, now);
    CHECK(r10.msgs.size() == 1);
    CHECK(r10.msgs[0].iface == "RANDR");

    std::vector<uint8_t> err(32, 0);
    err[0] = 0;
    err[1] = 3;
    err[2] = 1;
    err[3] = 0;
    auto r11 = p.feed(x11::DIR_S2C, err.data(), err.size(), 0, now);
    CHECK(r11.msgs.size() == 1);
    CHECK(r11.msgs[0].name == "Error:Window");

    auto s = p.summary();
    CHECK_CONTAIN(s["x11_extensions"], "RANDR(op=140");
    CHECK_CONTAIN(s["server_vendor"], "Test");

    x11_chunked_and_big();
}

void x11_chunked_and_big() {
    X11Parser p;
    TimePoint now = Clock::now();
    auto cs = csetup();
    p.feed(x11::DIR_C2S, cs.data(), cs.size(), 0, now);

    auto full = xreq(8, 0, [&] {
        std::vector<uint8_t> v;
        put_u32le(v, 0x1234);
        return v;
    }());
    auto a = p.feed(x11::DIR_C2S, full.data(), 5, 0, now);
    CHECK(a.msgs.empty());
    auto b = p.feed(x11::DIR_C2S, full.data() + 5, full.size() - 5, 0, now);
    CHECK(b.msgs.size() == 1);
    CHECK(b.msgs[0].name == "MapWindow");

    std::vector<uint8_t> big;
    big.push_back(254);
    big.push_back(72);
    big.push_back(0);
    big.push_back(0);
    uint32_t total_units = uint32_t((8 + 404) / 4);
    put_u32le(big, total_units);
    for (int i = 0; i < 404; i++) big.push_back(0);
    auto c = p.feed(x11::DIR_C2S, big.data(), big.size(), 0, now);
    CHECK(c.msgs.size() == 1);
    CHECK(c.msgs[0].name == "PutImage");
    CHECK(c.msgs[0].bytes == int(big.size()));
}
