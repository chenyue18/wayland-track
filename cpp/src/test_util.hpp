#pragma once

#include <cstdio>
#include <string>

extern int g_failures;
extern int g_checks;

#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        g_checks++;                                                                                   \
        if (!(cond)) {                                                                                \
            g_failures++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
        }                                                                                             \
    } while (0)

#define CHECK_CONTAIN(hay, needle)                                                                    \
    do {                                                                                              \
        g_checks++;                                                                                   \
        if (std::string(hay).find(needle) == std::string::npos) {                                     \
            g_failures++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: \"%s\" 应包含 \"%s\"\n", __FILE__, __LINE__,                  \
                    std::string(hay).c_str(), needle);                                                \
        }                                                                                             \
    } while (0)
