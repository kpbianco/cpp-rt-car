#include <simcore/SimCore.hpp>

int main() {
  auto worker = std::thread([] {});
  worker.join();
  return 0;
}
