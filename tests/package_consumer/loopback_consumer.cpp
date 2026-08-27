#include <rt/loopback_backend.hpp>

#include <array>
#include <cstddef>

int main() {
    rt::SampledIoLoopbackBackend backend;
    if (backend.add_route({17, 101, 202, 7, 303, 404}) != rt::Status::ok) {
        return 1;
    }
    const auto registration = backend.hal_v2_registration(
        "installed.sampled.loopback");
    if (!registration.api.instance || !registration.memory_topology ||
        !registration.command_timeline) {
        return 2;
    }
    const std::array payload{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    return rt::sampled_io_encoding_bytes(
               rt::SampledIoEncoding::signed_int16_le) == 2 &&
            rt::sampled_io_payload_checksum(payload) != 0
        ? 0
        : 3;
}
