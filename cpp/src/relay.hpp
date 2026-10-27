#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "report.hpp"

struct PumpCtx {
    int client_fd = -1;
    int real_fd = -1;
    std::shared_ptr<Parser> parser;
    std::shared_ptr<Tracker> tracker;
    uint64_t conn_id = 0;
    std::shared_ptr<std::atomic<int>> remaining;
};

namespace relay {
enum { DIR_C2S = 0, DIR_S2C = 1 };

void pump(int src_fd, int dst_fd, int dir, std::shared_ptr<PumpCtx> ctx);
void pump_impl(int src_fd, int dst_fd, int dir, std::shared_ptr<PumpCtx> ctx);
}  // namespace relay
