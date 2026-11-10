#include <FlowmeterComms.hpp>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        comms::FlowmeterConfig config;
        config.simulate = true;
        config.simulation_hz = 100;
        config.pulses_per_liter = 100;
        comms::FlowmeterComms driver(config);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        uint64_t previous = 0;
        while (driver.snapshot().pulse_count < 10 && std::chrono::steady_clock::now() < deadline) {
            auto state = driver.snapshot();
            check(state.pulse_count >= previous, "Count regressed");
            previous = state.pulse_count;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        driver.stop();
        auto final = driver.snapshot();
        check(final.pulse_count >= 10, "Simulation did not reach consumer");
        check(final.calibrated && std::abs(final.volume_liters - final.pulse_count / 100.0) < 1e-9, "Calibration incorrect");
        check(final.pulse_frequency_hz > 0 && final.pulse_frequency_hz < 1000, "Frequency invalid");
        check(std::abs(final.flow_liters_per_minute - final.pulse_frequency_hz * 0.6) < 1e-9, "Flow conversion incorrect");
        check(final.missed_pulses == 0 && !driver.failed(), "Unexpected producer failure");
        driver.stop();
        check(driver.snapshot().pulse_count == final.pulse_count, "Count changed after join");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        check(driver.snapshot().pulse_frequency_hz == 0, "Stale flow not zeroed");
        config.simulation_hz = 0.01;
        config.pulses_per_liter = 0;
        auto begin = std::chrono::steady_clock::now();
        { comms::FlowmeterComms idle(config); check(!idle.snapshot().calibrated, "False calibration"); }
        check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1), "Idle shutdown blocked");
        for (double rate : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
            config.simulation_hz = rate;
            bool rejected = false;
            try { comms::FlowmeterComms invalid(config); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "Invalid rate accepted");
        }
        for (int i = 0; i < 50; ++i) {
            config.simulation_hz = 1000;
            comms::FlowmeterComms short_lived(config);
        }
        std::cout << "PASS: pulse delivery, calibration, snapshots, stale flow, idle shutdown, invalid config, 50 lifecycle cycles\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
