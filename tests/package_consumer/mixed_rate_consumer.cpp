#include "../mixed_rate_conformance.hpp"

int main() {
    const auto result = rtfw_test::run_mixed_rate_conformance();
    return result.status == rt::Status::ok &&
            result.callback_counts[0] == 9 &&
            result.callback_counts[1] == 6 &&
            result.callback_counts[2] == 4 &&
            result.callback_counts[3] == 4 &&
            result.callback_counts[4] == 2 &&
            result.device_terminal_actions == 10 &&
            result.sampled_publish_actions == 23 &&
            result.sampled_select_actions == 16 &&
            result.startup_safe_acknowledged &&
            result.shutdown_safe_acknowledged &&
            result.active_replay_exact &&
            result.memory_accounting_exact
        ? 0
        : 1;
}
