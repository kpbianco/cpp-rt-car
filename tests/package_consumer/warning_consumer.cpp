#include <rt/runtime.hpp>

#if defined(LOG_ENABLED) || defined(LOG_DEFAULT_LEVEL) || \
    defined(PROF_ENABLED) || defined(SIM_WERROR)
#error "RTFW project policy leaked into an installed consumer"
#endif

int main() {
    int intentionally_unused = 0;
    return rt::query_capabilities().compiled_graph ? 0 : 1;
}
