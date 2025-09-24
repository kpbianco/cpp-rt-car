#include <simcore/metrics.hpp>

namespace metrics {

std::string Metrics::snapshot(bool reset) { return to_json(reset); }

} // namespace metrics
