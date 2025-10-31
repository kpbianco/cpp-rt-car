#pragma once

// BLE link priority helper for RaceChrono sketches.
//
// These helpers wrap a NimBLE server callback that requests a short
// connection interval and negotiates a larger MTU when a central connects.
// They are written so the same sketch can still build in non-Arduino
// configurations: every function becomes a no-op unless ARDUINO is defined.

namespace racechrono::ble_link_priority
{

/**
 * Initialise the optional BLE link priority helpers.
 *
 * Call this early in your sketch (before bringing the RaceChrono BLE
 * service online). When running on Arduino it sets the preferred MTU and
 * attaches the connection callbacks as soon as the NimBLE server exists.
 */
void setup();

/**
 * Periodic maintenance. Invoke from loop() so the helper can attach to a
 * server that came up late and fire the one-shot retry timer.
 */
void loop();

/**
 * Notify the helper that the RaceChrono BLE server is ready. This is safe to
 * call repeatedly. It simply gives the helper an opportunity to attach its
 * callbacks immediately instead of waiting for the next loop() poll.
 */
void notifyServerReady();

/** Enable or disable the link priority feature at runtime. */
void setEnabled(bool enabled);

/** Returns the current enabled state. */
bool isEnabled();

} // namespace racechrono::ble_link_priority

