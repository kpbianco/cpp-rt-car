#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/rt_memory.hpp>
#include <simcore/soa/first_touch.hpp>

#ifdef SIM_USE_NUMA
#include <numa.h>
#include <sched.h>

namespace {
struct ArenaCapture {
  void push(void *addr, int node) {
    std::lock_guard<std::mutex> lock(mutex);
    addresses.push_back(addr);
    nodes.push_back(node);
  }

  std::mutex mutex;
  std::vector<void *> addresses;
  std::vector<int> nodes;
};

void expect_pages_on_nodes(void *base, std::size_t bytes,
                           std::size_t firstBytes, int nodeA, int nodeB) {
  if (bytes == 0)
    return;
  const std::size_t page = rt::os_page_size();
  const std::size_t totalPages = (bytes + page - 1) / page;
  std::vector<void *> pages(totalPages);
  for (std::size_t i = 0; i < totalPages; ++i)
    pages[i] = static_cast<char *>(base) + i * page;
  std::vector<int> status(totalPages);
  errno = 0;
  const long rc =
      numa_move_pages(0, static_cast<unsigned long>(totalPages), pages.data(),
                      nullptr, status.data(), 0);
  ASSERT_EQ(rc, 0) << "numa_move_pages failed: " << std::strerror(errno);
  const std::size_t boundaryPages =
      std::min((firstBytes + page - 1) / page, totalPages);
  for (std::size_t i = 0; i < boundaryPages; ++i)
    EXPECT_EQ(status[i], nodeA);
  for (std::size_t i = boundaryPages; i < totalPages; ++i)
    EXPECT_EQ(status[i], nodeB);
}

void expect_pages_on_single_node(void *base, std::size_t bytes, int node) {
  expect_pages_on_nodes(base, bytes, bytes, node, node);
}
} // namespace

TEST(NumaIntegration, WorkerArenasAreNodeLocal) {
  if (numa_available() < 0 || numa_num_configured_nodes() < 2) {
    GTEST_SKIP() << "NUMA not available";
  }
  if (std::thread::hardware_concurrency() < 2) {
    GTEST_SKIP() << "Insufficient hardware concurrency";
  }

  SimCore::Settings cfg;
  cfg.threads = 2;
  cfg.pinThreads = true;
  cfg.compactNUMA = true;
  cfg.maxFrames = 1;
  cfg.autoTuneChunks = false;
  cfg.chunkSize = 1;
  cfg.arenaPerThreadBytes = 1u << 12;

  SimCore sim(cfg);
  std::size_t phase = sim.addPhase("numa", cfg.threads * 8);

  ArenaCapture capture;
  sim.addParallelRangeTask(
      phase, [&sim, &capture](std::size_t, std::size_t, std::int64_t,
                              SimCore::Seconds) {
        static thread_local bool recorded = false;
        if (recorded)
          return;
        recorded = true;
        void *ptr = sim.frameArena().allocate(4096);
        std::memset(ptr, 1, 4096);
        int cpu = sched_getcpu();
        int node = (cpu >= 0) ? numa_node_of_cpu(cpu) : -1;
        capture.push(ptr, node);
      });

  sim.run();

  ASSERT_FALSE(capture.addresses.empty());
  ASSERT_GE(capture.addresses.size(), cfg.threads);
  std::vector<void *> pages = capture.addresses;
  std::vector<int> status(pages.size());
  numa_move_pages(0, static_cast<unsigned long>(pages.size()), pages.data(),
                  nullptr, status.data(), 0);

  for (std::size_t i = 0; i < pages.size(); ++i) {
    ASSERT_GE(capture.nodes[i], 0);
    EXPECT_EQ(status[i], capture.nodes[i]);
  }
}

TEST(NumaIntegration, ThreadArenaFirstTouchMatchesRuntimeNode) {
  if (numa_available() < 0 || numa_num_configured_nodes() < 2) {
    GTEST_SKIP() << "NUMA not available";
  }

  const int maxNode = numa_max_node();
  struct bitmask *cpuMask = numa_allocate_cpumask();
  if (!cpuMask) {
    GTEST_SKIP() << "Failed to allocate NUMA CPU mask";
  }

  std::vector<int> nodes;
  nodes.reserve(static_cast<std::size_t>(maxNode + 1));
  for (int node = 0; node <= maxNode; ++node) {
    if (numa_node_to_cpus(node, cpuMask) != 0)
      continue;
    for (unsigned long bit = 0; bit < cpuMask->size; ++bit) {
      if (numa_bitmask_isbitset(cpuMask, static_cast<unsigned int>(bit))) {
        nodes.push_back(node);
        break;
      }
    }
  }
  numa_free_cpumask(cpuMask);

  if (nodes.size() < 2) {
    GTEST_SKIP() << "Less than two NUMA nodes with online CPUs";
  }

  const std::size_t threads = nodes.size();
  const std::size_t bytesPerThread = 1u << 20; // 1 MiB per arena
  const std::size_t valuesPerThread = bytesPerThread / sizeof(double);

  rt::FrameArenaPool pool(threads, bytesPerThread, 64, nodes);
  std::vector<double *> allocations(threads, nullptr);
  std::vector<int> runtimeNodes(threads, -1);

  rt::numa::parallel_for_nodes(
      threads, nodes,
      [&](std::size_t begin, std::size_t end, int assignedNode) {
        for (std::size_t idx = begin; idx < end; ++idx) {
          pool.bindCurrentThread(idx);
          double *data = pool.tls().allocateArray<double>(valuesPerThread);
          for (std::size_t j = 0; j < valuesPerThread; ++j)
            data[j] = static_cast<double>(idx);
          allocations[idx] = data;
          int cpu = sched_getcpu();
          int runtimeNode = assignedNode;
          if (cpu >= 0) {
            const int cpuNode = numa_node_of_cpu(cpu);
            if (cpuNode >= 0)
              runtimeNode = cpuNode;
          }
          runtimeNodes[idx] = runtimeNode;
        }
      });

  for (std::size_t i = 0; i < threads; ++i) {
    ASSERT_NE(allocations[i], nullptr);
    ASSERT_GE(runtimeNodes[i], 0);
    expect_pages_on_single_node(allocations[i], bytesPerThread,
                                runtimeNodes[i]);
    EXPECT_DOUBLE_EQ(allocations[i][0], static_cast<double>(i));
  }
}

TEST(NumaIntegration, FirstTouchSoADistributesPages) {
  if (numa_available() < 0 || numa_num_configured_nodes() < 2) {
    GTEST_SKIP() << "NUMA not available";
  }

  const std::vector<int> nodes{0, 1};
  constexpr std::size_t elements = 1u << 19; // ~4 MiB per channel
  auto x = std::make_unique<double[]>(elements);
  auto y = std::make_unique<double[]>(elements);
  auto z = std::make_unique<double[]>(elements);
  soa::Vec3SoA<double> soa{x.get(), y.get(), z.get()};

  soa::init_vec3_soa(soa, elements, nodes, 0.0);

  const std::size_t totalBytes = elements * sizeof(double);
  const std::size_t threads = nodes.size();
  const std::size_t chunk = (elements + threads - 1) / threads;
  const std::size_t firstElems = std::min(elements, chunk);
  const std::size_t firstBytes = firstElems * sizeof(double);

  expect_pages_on_nodes(x.get(), totalBytes, firstBytes, nodes[0], nodes[1]);
  EXPECT_DOUBLE_EQ(x[0], 0.0);
  EXPECT_DOUBLE_EQ(y[0], 0.0);
  EXPECT_DOUBLE_EQ(z[0], 0.0);
}

TEST(NumaIntegration, FirstTouchAoSoADistributesPages) {
  if (numa_available() < 0 || numa_num_configured_nodes() < 2) {
    GTEST_SKIP() << "NUMA not available";
  }

  const std::vector<int> nodes{0, 1};
  constexpr std::size_t Tile = 256;
  constexpr std::size_t elements = Tile * 512; // 512 tiles worth of elements
  const std::size_t tileCount = (elements + Tile - 1) / Tile;
  using TileType = typename soa::Vec3AoSoA<double, Tile>::Tile;
  auto tiles = std::make_unique<TileType[]>(tileCount);
  soa::Vec3AoSoA<double, Tile> aosoa{tiles.get()};

  soa::init_vec3_aosoa(aosoa, elements, nodes, 1.0);

  const std::size_t totalBytes = tileCount * sizeof(TileType);
  const std::size_t totalSlots = tileCount * Tile;
  const std::size_t threads = nodes.size();
  const std::size_t chunk = (totalSlots + threads - 1) / threads;
  const std::size_t firstElems = std::min(totalSlots, chunk);
  const std::size_t fullTiles = firstElems / Tile;
  const std::size_t lane = firstElems % Tile;
  std::size_t firstBytes = fullTiles * sizeof(TileType);
  if (lane > 0)
    firstBytes += (2 * Tile + lane) * sizeof(double);

  expect_pages_on_nodes(tiles.get(), totalBytes, firstBytes, nodes[0], nodes[1]);
  EXPECT_DOUBLE_EQ(aosoa.tiles[0].x[0], 1.0);
  EXPECT_DOUBLE_EQ(aosoa.tiles[0].y[0], 1.0);
  EXPECT_DOUBLE_EQ(aosoa.tiles[0].z[0], 1.0);
}
#else
TEST(NumaIntegration, Skipped) {
  GTEST_SKIP() << "SIM_USE_NUMA not enabled";
}
#endif

