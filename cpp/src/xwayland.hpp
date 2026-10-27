#pragma once

#include <string>
#include <vector>

#include "daemon.hpp"

struct XwaylandOptions {
    DaemonOptions base;
    std::vector<std::string> command;
    int display = -1;
};

int run_xwayland(const XwaylandOptions& opts);
