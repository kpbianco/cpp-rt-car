#include <rt/c_api.h>

int main(void) {
    rtfw_abi_info info;
    rtfw_abi_info_init(&info);
    if (rtfw_get_abi_info(&info) != RTFW_STATUS_OK ||
        info.abi_version != RTFW_C_ABI_VERSION ||
        info.layout_fingerprint != RTFW_C_ABI_LAYOUT_FINGERPRINT) {
        return 1;
    }
    return 0;
}
