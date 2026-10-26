#pragma once

#include <cstdio>
#include <string>

void set_log_file(const std::string& path);
void logf(const char* f, ...) __attribute__((format(printf, 1, 2)));
