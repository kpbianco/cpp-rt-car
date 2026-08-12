#include <rt/runtime.hpp>

#include <cstring>

namespace {

rtfw_status RTFW_EXTENSION_CALL entry(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor) {
    if (!host || host->current_abi_version != 1u) {
        return RTFW_STATUS_INCOMPATIBLE_ABI;
    }
    *descriptor = {};
    descriptor->struct_size = RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE;
    descriptor->current_abi_version = 1;
    descriptor->min_compatible_abi_version = 1;
    std::memcpy(descriptor->name, "installed.extension", 20);
    std::memcpy(descriptor->version, "1.0", 4);
    return RTFW_STATUS_OK;
}

} // namespace

int main() {
    rt::Runtime runtime;
    rt::ExtensionHandle extension;
    if (runtime.register_extension(&entry, extension) != rt::Status::ok ||
        !extension.valid() || runtime.finalize() != rt::Status::ok ||
        runtime.stop() != rt::Status::ok) {
        return 1;
    }
    bool ready = false;
    return runtime.detach_extension(extension, ready) == rt::Status::ok && ready
        ? 0
        : 2;
}
