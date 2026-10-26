#pragma once

#include <string>

#include "util.hpp"

struct ProcInfo {
    int pid = 0;
    std::string comm;
    std::string exe;
    std::string cmdline;
};

ProcInfo procinfo_read(int pid);
bool procinfo_alive(int pid);
