#include <racechrono/ble_link_priority.hpp>

#ifdef ARDUINO

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <cstdint>

namespace racechrono::ble_link_priority
{
namespace
{

constexpr std::uint16_t kConnIntervalMin = 6;   // 7.5 ms (1.25 ms units)
constexpr std::uint16_t kConnIntervalMax = 12;  // 15 ms
constexpr std::uint16_t kConnLatency = 0;
constexpr std::uint16_t kSupervisionTimeout = 400; // 4 s (10 ms units)
constexpr std::uint16_t kInvalidConnHandle = 0xFFFF;

bool gEnabled = true;
bool gRetryScheduled = false;
std::uint32_t gRetryDeadlineMs = 0;
std::uint16_t gPendingHandle = kInvalidConnHandle;
std::uint16_t gLastReportedMtu = 0;
NimBLEServer* gServer = nullptr;

class LinkPriorityCallbacks : public NimBLEServerCallbacks
{
public:
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override
    {
        gServer = server;
        gPendingHandle = connInfo.getConnHandle();
        logMtu(connInfo.getMTU());
        requestUpdate("ConnParams req", gPendingHandle);
        scheduleRetry();
    }

    void onConnect(NimBLEServer* server) override
    {
        // Fallback for older NimBLE versions that do not provide NimBLEConnInfo
        gServer = server;
        scheduleRetry();
    }

    void onDisconnect(NimBLEServer* /*server*/) override
    {
        gPendingHandle = kInvalidConnHandle;
        gRetryScheduled = false;
    }

    void onMTUChange(std::uint16_t mtu, NimBLEConnInfo& /*connInfo*/) override
    {
        logMtu(mtu);
    }

private:
    static void logMtu(std::uint16_t mtu)
    {
        if (mtu == 0 || mtu == gLastReportedMtu)
            return;
        gLastReportedMtu = mtu;
        Serial.printf("MTU negotiated: %u\n", static_cast<unsigned>(mtu));
    }

    static void requestUpdate(const char* prefix, std::uint16_t connHandle)
    {
        if (!gEnabled || gServer == nullptr || connHandle == kInvalidConnHandle)
            return;

        const bool sent = gServer->updateConnParams(connHandle,
                                                    kConnIntervalMin,
                                                    kConnIntervalMax,
                                                    kConnLatency,
                                                    kSupervisionTimeout);
        Serial.printf("%s: min=7.50 ms max=15.00 ms latency=%u timeout=%.2f s -> %s\n",
                      prefix,
                      static_cast<unsigned>(kConnLatency),
                      static_cast<float>(kSupervisionTimeout) * 0.01f,
                      sent ? "sent" : "rejected");
    }

    static void scheduleRetry()
    {
        if (!gEnabled || gPendingHandle == kInvalidConnHandle)
            return;
        gRetryScheduled = true;
        gRetryDeadlineMs = millis() + 2000u;
    }

public:
    static void maybeRetry()
    {
        if (!gRetryScheduled)
            return;
        if (static_cast<std::int32_t>(millis() - gRetryDeadlineMs) < 0)
            return;
        gRetryScheduled = false;
        requestUpdate("ConnParams retry", gPendingHandle);
    }
};

LinkPriorityCallbacks gCallbacks;

void attachIfReady()
{
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server == nullptr)
        return;
    if (gServer == server)
        return;
    gServer = server;
    gLastReportedMtu = 0;
    server->setCallbacks(&gCallbacks, false);
}

} // namespace

void setup()
{
    NimBLEDevice::setMTU(185);
    attachIfReady();
}

void loop()
{
    attachIfReady();
    LinkPriorityCallbacks::maybeRetry();
}

void notifyServerReady()
{
    attachIfReady();
}

void setEnabled(bool enabled)
{
    if (gEnabled == enabled)
        return;
    gEnabled = enabled;
    if (!enabled)
    {
        gRetryScheduled = false;
    }
    else if (gPendingHandle != kInvalidConnHandle)
    {
        requestUpdate("ConnParams req", gPendingHandle);
        scheduleRetry();
    }
}

bool isEnabled()
{
    return gEnabled;
}

} // namespace racechrono::ble_link_priority

#else  // !ARDUINO

namespace racechrono::ble_link_priority
{
namespace
{
bool gEnabled = true;
}

void setup() {}
void loop() {}
void notifyServerReady() {}
void setEnabled(bool enabled) { gEnabled = enabled; }
bool isEnabled() { return gEnabled; }

} // namespace racechrono::ble_link_priority

#endif

