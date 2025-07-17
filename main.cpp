#include "SimCore.hpp"
#include <iostream>

int main() {
    SimCore::Settings cfg;
    cfg.hz = 120.0;             // simulation frequency
    cfg.maxFrames = 240;        // number of frames before stopping
    cfg.adaptive = false;       // disable catch-up
    cfg.maxCatchUp = 0;

    SimCore sim(cfg);

    // Add a simple dummy subsystem to test the loop
    sim.addSubsystem([](int64_t frame, SimCore::Seconds dt) {
        std::cout << "Frame " << frame << " - dt: " << dt.count() << "s\n";
    });

    sim.run();  // start simulation loop

    return 0;
}
