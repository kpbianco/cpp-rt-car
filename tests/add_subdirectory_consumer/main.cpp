#include <rt/runtime.hpp>

int main() {
    return rt::query_capabilities().compiled_graph ? 0 : 1;
}
