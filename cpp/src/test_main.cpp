#include <cstdio>

int g_failures = 0;
int g_checks = 0;

void run_test_wayland();
void run_test_x11();
void run_test_xml();
void run_test_integration();

int main() {
    run_test_wayland();
    run_test_x11();
    run_test_xml();
    run_test_integration();
    printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
