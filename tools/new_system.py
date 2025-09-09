#!/usr/bin/env python3
"""Generate a new system skeleton with tests and benchmarks."""

import argparse
import pathlib
import textwrap

TEMPLATE_MAIN = """#include <iostream>

int main() {
    std::cout << "Hello from {name}!\n";
    return 0;
}
"""

TEMPLATE_TEST = """#include <gtest/gtest.h>

TEST({name}, Basic) {
    // TODO: add meaningful tests
    EXPECT_EQ(1, 1);
}
"""

TEMPLATE_BENCH = """// TODO: add benchmarks for {name}
"""

def main():
    parser = argparse.ArgumentParser(description="Generate new system skeleton")
    parser.add_argument("name", help="System name")
    args = parser.parse_args()

    root = pathlib.Path(args.name)
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "tests").mkdir(parents=True, exist_ok=True)
    (root / "bench").mkdir(parents=True, exist_ok=True)

    (root / "src" / f"{args.name}.cpp").write_text(TEMPLATE_MAIN.format(name=args.name))
    (root / "tests" / f"test_{args.name}.cpp").write_text(TEMPLATE_TEST.format(name=args.name))
    (root / "bench" / f"{args.name}_bench.cpp").write_text(TEMPLATE_BENCH.format(name=args.name))

    print(f"Generated skeleton for {args.name} in {root.resolve()}")

if __name__ == "__main__":
    main()
