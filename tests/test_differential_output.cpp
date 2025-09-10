#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

static std::vector<double> run_kernel() {
  std::vector<double> out;
  for (int i = 0; i < 10; ++i) {
    out.push_back(std::sin(i * 0.1));
  }
  return out;
}

static double max_abs_diff(const std::vector<double> &a,
                           const std::vector<double> &b) {
  double max_diff = 0.0;
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    max_diff = std::max(max_diff, std::abs(a[i] - b[i]));
  }
  if (a.size() != b.size()) {
    max_diff = std::max(max_diff, 1.0);
  }
  return max_diff;
}

TEST(DifferentialKernelTest, DriftBelowThreshold) {
  std::string baseline_path = std::string(PROJECT_SOURCE_DIR) +
                              "/tests/golden/differential_baseline.txt";
  std::ifstream in(baseline_path);
  ASSERT_TRUE(in) << "Failed to open baseline file";
  std::vector<double> baseline;
  double v;
  while (in >> v) {
    baseline.push_back(v);
  }

  auto current = run_kernel();
  double drift = max_abs_diff(baseline, current);
  EXPECT_LT(drift, 1e-5) << "max drift=" << drift;
}
