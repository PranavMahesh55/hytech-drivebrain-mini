#include <FlowmeterComms.hpp>
#include <FoxgloveServer.hpp>
#include <MCAPLogger.hpp>
#include <drivebrain_msgs.pb.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; } // Only async-signal-safe work here.
struct Options {
    comms::FlowmeterConfig sensor;
    std::string recordings = "/home/nixos/recordings";
    double duration = 0;
};
double number(const std::string& value) {
    size_t consumed = 0;
    double n = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(n)) throw std::invalid_argument("Invalid number: " + value);
    return n;
}
Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--simulate") { options.sensor.simulate = true; continue; }
        if (arg == "--falling") { options.sensor.falling = true; continue; }
        if (arg == "--help") {
            std::cout << "mini-drivebrain [--simulate] [--hz 20] [--chip /dev/gpiochip0] [--line 12]\n"
                         "  [--falling] [--pulses-per-liter N] [--recordings DIR] [--duration SECONDS]\n"
                         "Default mode reads real GPIO. Calibration is disabled until supplied.\n";
            std::exit(0);
        }
        if (i + 1 == argc) throw std::invalid_argument("Missing value for " + arg);
        std::string value = argv[++i];
        if (arg == "--chip") options.sensor.chip = value;
        else if (arg == "--recordings") options.recordings = value;
        else if (arg == "--hz") options.sensor.simulation_hz = number(value);
        else if (arg == "--duration") { options.duration = number(value); if (options.duration <= 0) throw std::invalid_argument("Duration must be positive"); }
        else if (arg == "--pulses-per-liter") { options.sensor.pulses_per_liter = number(value); if (options.sensor.pulses_per_liter <= 0) throw std::invalid_argument("Calibration must be positive"); }
        else if (arg == "--line") {
            double line = number(value);
            if (line < 0 || line > std::numeric_limits<unsigned>::max() || std::floor(line) != line)
                throw std::invalid_argument("Line must be an unsigned integer offset");
            options.sensor.line = static_cast<unsigned>(line);
        } else throw std::invalid_argument("Unknown option " + arg);
    }
    return options;
}
void publish(const comms::FlowmeterSnapshot& state, bool simulated) {
    auto message = std::make_shared<drivebrain_msgs::FlowmeterTelemetry>();
    message->set_timestamp_unix_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    message->set_pulse_count(state.pulse_count);
    message->set_last_pulse_monotonic_ns(state.last_pulse_monotonic_ns);
    message->set_pulse_frequency_hz(state.pulse_frequency_hz);
    message->set_calibrated(state.calibrated);
    message->set_volume_liters(state.volume_liters);
    message->set_flow_liters_per_minute(state.flow_liters_per_minute);
    message->set_missed_pulses(state.missed_pulses);
    message->set_simulated(simulated);
    if (core::MCAPLogger::instance().log_msg(message, "/flowmeter") != 0)
        throw std::runtime_error("MCAP logging failed");
    core::FoxgloveServer::instance().send_live_telem_msg(message, "/flowmeter");
}
}
int main(int argc, char** argv) {
    int result = 0;
    try {
        auto options = parse(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        core::MCAPLogger::create(mcap::McapWriterOptions(""), options.recordings);
        if (core::MCAPLogger::instance().open_new_mcap() != 0) throw std::runtime_error("Cannot open recording");
        core::MCAPLogger::instance().init_logging();
        core::FoxgloveServer::create();
        // Local driver is destroyed during stack unwinding before global helpers below.
        comms::FlowmeterComms flowmeter(options.sensor);
        spdlog::info("Flowmeter started (simulated={}), /flowmeter on port 5555", options.sensor.simulate);
        auto start = std::chrono::steady_clock::now();
        auto next = start;
        while (!interrupted) {
            if (flowmeter.failed()) throw std::runtime_error("Sensor worker failed; check GPIO connection and journal");
            if (options.duration > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= options.duration) break;
            publish(flowmeter.snapshot(), options.sensor.simulate);
            next += std::chrono::milliseconds(20); // 50 Hz snapshots; edges are counted independently.
            if (next < std::chrono::steady_clock::now()) next = std::chrono::steady_clock::now();
            std::this_thread::sleep_until(next);
        }
        flowmeter.stop(); // Join producer, drain consumer, then capture final count.
        if (flowmeter.failed()) throw std::runtime_error("Sensor worker failed during shutdown");
        auto final_state = flowmeter.snapshot();
        publish(final_state, options.sensor.simulate);
        spdlog::info("Final pulse count: {}, missed: {}", final_state.pulse_count, final_state.missed_pulses);
    } catch (const std::exception& error) {
        std::cerr << "mini-drivebrain: " << error.what() << '\n';
        result = 1;
    }
    core::FoxgloveServer::destroy();
    core::MCAPLogger::destroy(); // Drains messages and closes the file.
    if (core::MCAPLogger::write_failed()) result = 1;
    return result;
}
