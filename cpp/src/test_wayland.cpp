#include <cstring>

#include "proto_db.hpp"
#include "test_util.hpp"
#include "util.hpp"
#include "wayland.hpp"
#include <memory>

namespace {

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 24) & 0xff);
}

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
}

void put_str(std::vector<uint8_t>& v, const std::string& s) {
    put_u32(v, uint32_t(s.size() + 1));
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
    while (v.size() % 4) v.push_back(0);
}

std::vector<uint8_t> wl_msg(uint32_t obj, uint16_t opcode, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> v;
    put_u32(v, obj);
    put_u16(v, opcode);
    put_u16(v, uint16_t(8 + payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

}  // namespace

void run_test_wayland() {
    ProtoDB db;
    db.load_embedded();
    CHECK(db.iface_count() >= 48);

    WaylandParser p(std::make_shared<const ProtoDB>(db));
    TimePoint now = Clock::now();

    auto r1 = p.feed(wayland::DIR_C2S, wl_msg(1, 1, [&] {
        std::vector<uint8_t> v;
        put_u32(v, 2);
        return v;
    }()).data(), 12, 0, now);
    CHECK(r1.msgs.size() == 1);
    CHECK(r1.msgs[0].name == "get_registry");
    CHECK(r1.msgs[0].iface == "wl_display");
    CHECK_CONTAIN(r1.msgs[0].detail, "registry=wl_registry@2");

    std::vector<uint8_t> global_payload;
    put_u32(global_payload, 1);
    put_str(global_payload, "wl_compositor");
    put_u32(global_payload, 5);
    auto r2 = p.feed(wayland::DIR_S2C, wl_msg(2, 0, global_payload).data(), global_payload.size() + 8, 0, now);
    CHECK(r2.msgs.size() == 1);
    CHECK(r2.msgs[0].name == "global");
    CHECK_CONTAIN(r2.msgs[0].detail, "interface=\"wl_compositor\"");

    std::vector<uint8_t> bind_payload;
    put_u32(bind_payload, 1);
    put_str(bind_payload, "wl_compositor");
    put_u32(bind_payload, 5);
    put_u32(bind_payload, 6);
    auto r3 = p.feed(wayland::DIR_C2S, wl_msg(2, 0, bind_payload).data(), bind_payload.size() + 8, 0, now);
    CHECK(r3.msgs.size() == 1);
    CHECK(r3.msgs[0].name == "bind");
    CHECK_CONTAIN(r3.msgs[0].detail, "id=wl_compositor@6 v5");

    std::vector<uint8_t> surface_id;
    put_u32(surface_id, 7);
    auto r4 = p.feed(wayland::DIR_C2S, wl_msg(6, 0, surface_id).data(), 12, 0, now);
    CHECK(r4.msgs.size() == 1);
    CHECK_CONTAIN(r4.msgs[0].detail, "wl_surface@7");

    std::vector<uint8_t> attach;
    put_u32(attach, 8);
    put_u32(attach, 0);
    put_u32(attach, 0);
    auto r5 = p.feed(wayland::DIR_C2S, wl_msg(7, 1, attach).data(), 20, 0, now);
    CHECK(r5.msgs.size() == 1);
    CHECK(r5.msgs[0].name == "attach");

    auto r6 = p.feed(wayland::DIR_S2C, wl_msg(1, 1, [&] {
        std::vector<uint8_t> v;
        put_u32(v, 8);
        return v;
    }()).data(), 12, 0, now);
    CHECK(r6.msgs.size() == 1);
    CHECK(r6.msgs[0].name == "delete_id");

    auto r7 = p.feed(wayland::DIR_C2S, wl_msg(7, 1, attach).data(), 20, 0, now);
    CHECK(r7.msgs.size() == 1);
    CHECK_CONTAIN(r7.msgs[0].detail, "buffer=object@8");

    auto s = p.summary();
    CHECK_CONTAIN(s["registry_globals"], "wl_compositor");
    CHECK_CONTAIN(s["bound_objects"], "wl_compositor×1");

    // fd 参数
    WaylandParser p2(std::make_shared<const ProtoDB>(db));
    std::vector<uint8_t> gr;
    put_u32(gr, 2);
    p2.feed(wayland::DIR_C2S, wl_msg(1, 1, gr).data(), 12, 0, now);
    std::vector<uint8_t> bind_shm;
    put_u32(bind_shm, 1);
    put_str(bind_shm, "wl_shm");
    put_u32(bind_shm, 1);
    put_u32(bind_shm, 10);
    p2.feed(wayland::DIR_C2S, wl_msg(2, 0, bind_shm).data(), bind_shm.size() + 8, 0, now);
    std::vector<uint8_t> create_pool;
    put_u32(create_pool, 11);
    put_u32(create_pool, 4096);
    auto rf = p2.feed(wayland::DIR_C2S, wl_msg(10, 0, create_pool).data(), 16, 1, now);
    CHECK(rf.msgs.size() == 1);
    CHECK(rf.msgs[0].name == "create_pool");
    CHECK_CONTAIN(rf.msgs[0].detail, "fd=fd#0");
    CHECK_CONTAIN(rf.msgs[0].detail, "size=4096");

    // 分块输入
    WaylandParser p3(std::make_shared<const ProtoDB>(db));
    auto full = wl_msg(1, 1, [&] {
        std::vector<uint8_t> v;
        put_u32(v, 2);
        return v;
    }());
    auto half1 = p3.feed(wayland::DIR_C2S, full.data(), 5, 0, now);
    CHECK(half1.msgs.empty());
    auto half2 = p3.feed(wayland::DIR_C2S, full.data() + 5, full.size() - 5, 0, now);
    CHECK(half2.msgs.size() == 1);
    CHECK(half2.msgs[0].name == "get_registry");

    // enum 符号化解码: wl_seat.capabilities (bitfield)
    WaylandParser p5(std::make_shared<const ProtoDB>(db));
    std::vector<uint8_t> getreg;
    put_u32(getreg, 2);
    p5.feed(wayland::DIR_C2S, wl_msg(1, 1, getreg).data(), 12, 0, now);
    std::vector<uint8_t> seat_bind;
    put_u32(seat_bind, 1);
    put_str(seat_bind, "wl_seat");
    put_u32(seat_bind, 10);
    put_u32(seat_bind, 20);
    auto rb = p5.feed(wayland::DIR_C2S, wl_msg(2, 0, seat_bind).data(), seat_bind.size() + 8, 0, now);
    CHECK(rb.msgs.size() == 1);
    if (rb.msgs.size() == 1) {
        fprintf(stderr, "BIND: name=%s detail=%s\n", rb.msgs[0].name.c_str(), rb.msgs[0].detail.c_str());
        CHECK(rb.msgs[0].name == "bind");
    }
    std::vector<uint8_t> caps;
    put_u32(caps, 3);
    auto re = p5.feed(wayland::DIR_S2C, wl_msg(20, 0, caps).data(), 12, 0, now);
    CHECK(re.msgs.size() == 1);
    CHECK_CONTAIN(re.msgs[0].detail, "capabilities=pointer|keyboard");

    // 非位域: wl_shm.format
    std::vector<uint8_t> shm_bind;
    put_u32(shm_bind, 1);
    put_str(shm_bind, "wl_shm");
    put_u32(shm_bind, 2);
    put_u32(shm_bind, 30);
    p5.feed(wayland::DIR_C2S, wl_msg(2, 0, shm_bind).data(), shm_bind.size() + 8, 0, now);
    std::vector<uint8_t> fmt1;
    put_u32(fmt1, 1);
    auto rf2 = p5.feed(wayland::DIR_S2C, wl_msg(30, 0, fmt1).data(), 12, 0, now);
    CHECK(rf2.msgs.size() == 1);
    CHECK_CONTAIN(rf2.msgs[0].detail, "format=xrgb8888");

    // 未知枚举值回退数字
    std::vector<uint8_t> fmt9;
    put_u32(fmt9, 9);
    auto rf3 = p5.feed(wayland::DIR_S2C, wl_msg(30, 0, fmt9).data(), 12, 0, now);
    CHECK_CONTAIN(rf3.msgs[0].detail, "format=9");

    // 未知接口回退
    WaylandParser p4(std::make_shared<const ProtoDB>(db));
    std::vector<uint8_t> raw;
    put_u32(raw, 1);
    auto ru = p4.feed(wayland::DIR_S2C, wl_msg(300, 7, raw).data(), 12, 0, now);
    CHECK(ru.msgs.size() == 1);
    CHECK(ru.msgs[0].name == "opcode-7");
    CHECK_CONTAIN(ru.msgs[0].detail, "raw:");
}
