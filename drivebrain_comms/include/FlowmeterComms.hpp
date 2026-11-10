#pragma once
#include <DriverBus.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace comms {
struct Pulse { uint64_t timestamp_ns; uint64_t missed_before; };
struct FlowmeterConfig {
    std::string chip = "/dev/gpiochip0";
    unsigned line = 12;
    bool falling = false;
    bool simulate = false;
    double simulation_hz = 20;
    double pulses_per_liter = 0; // Zero means uncalibrated; do not report volume.
};
struct FlowmeterSnapshot {
    uint64_t pulse_count = 0;
    uint64_t last_pulse_monotonic_ns = 0;
    uint64_t missed_pulses = 0;
    double pulse_frequency_hz = 0;
    double volume_liters = 0;
    double flow_liters_per_minute = 0;
    bool calibrated = false;
};
// One-shot object. stop() is idempotent, called from the owning thread only.
class FlowmeterComms {
public:
    explicit FlowmeterComms(FlowmeterConfig config);
    ~FlowmeterComms();
    FlowmeterComms(const FlowmeterComms&) = delete;
    FlowmeterComms& operator=(const FlowmeterComms&) = delete;
    void stop() noexcept;
    FlowmeterSnapshot snapshot() const;
    bool failed() const noexcept { return failed_.load(); }
private:
    void read_pulses() noexcept;
    void process_pulses() noexcept;
    void enqueue(Pulse pulse);
    FlowmeterConfig config_;
    core::ThreadSafeDeque<Pulse> bus_;
    bool producer_done_ = false; // Protected by bus_.mtx.
    std::atomic<bool> stopping_{false}, failed_{false};
    mutable std::mutex state_mutex_;
    FlowmeterSnapshot state_;
    uint64_t previous_ns_ = 0;
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;
#ifdef __linux__
    int event_fd_ = -1;
#endif
    std::thread reader_, processor_;
};
}
