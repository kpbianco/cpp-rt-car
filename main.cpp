#include "SimCore.hpp"
#include <iostream>
#include <cmath>

int main(int argc, char* argv[])
{
    SimCore::Settings cfg;
    if (argc > 1) cfg.hz = std::stod(argv[1]);   // e.g.  ./sim 100000

    SimCore sim(cfg);

    sim.addSubsystem([&](std::int64_t f, SimCore::Seconds dt)
    {
        if (f % static_cast<std::int64_t>(sim.hz()) == 0)
            std::cout << "frame " << f
                      << "   dt " << dt.count()
                      << "   hz " << sim.hz() << '\n';

        // stop after 5 s of simulation time
        if (f >= 5 * static_cast<std::int64_t>(sim.hz()))
            sim.requestExit();
    });

    // physics subsystems go here
    sim.addSubsystem([](auto /*f*/, auto /*dt*/) {
        /* integrate car state */
    });

    sim.run();          // pass <true> to template for naive multithread demo
    return 0;
}
