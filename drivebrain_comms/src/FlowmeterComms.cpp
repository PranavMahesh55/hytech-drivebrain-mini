#include <FlowmeterComms.hpp>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef __linux__
#include <linux/gpio.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#endif

namespace {
uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
namespace comms {
FlowmeterComms::FlowmeterComms(FlowmeterConfig config) : config_(std::move(config)) {
    if (!std::isfinite(config_.simulation_hz) || config_.simulation_hz <= 0 || config_.simulation_hz > 1000000 ||
        !std::isfinite(config_.pulses_per_liter) || config_.pulses_per_liter < 0)
        throw std::invalid_argument("Invalid pulse rate or calibration");
    state_.calibrated = config_.pulses_per_liter > 0;
    if (!config_.simulate) {
#ifdef __linux__
        int chip = ::open(config_.chip.c_str(), O_RDONLY | O_CLOEXEC);
        if (chip < 0) throw std::system_error(errno, std::generic_category(), "Open GPIO chip");
        gpio_v2_line_request request{};
        request.offsets[0] = config_.line;
        request.num_lines = 1;
        request.event_buffer_size = 256;
        request.config.flags = GPIO_V2_LINE_FLAG_INPUT |
            (config_.falling ? GPIO_V2_LINE_FLAG_EDGE_FALLING : GPIO_V2_LINE_FLAG_EDGE_RISING);
        std::snprintf(request.consumer, sizeof(request.consumer), "mini-drivebrain");
        int result = ::ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request);
        int error = errno;
        ::close(chip);
        if (result < 0) throw std::system_error(error, std::generic_category(), "Request GPIO v2 line");
        event_fd_ = request.fd;
        int flags = ::fcntl(event_fd_, F_GETFL);
        if (flags < 0 || ::fcntl(event_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            error = errno; ::close(event_fd_); event_fd_ = -1;
            throw std::system_error(error, std::generic_category(), "Configure GPIO nonblocking mode");
        }
#else
        throw std::runtime_error("GPIO requires Linux; use --simulate on this platform");
#endif
    }
    try {
        processor_ = std::thread(&FlowmeterComms::process_pulses, this);
        reader_ = std::thread(&FlowmeterComms::read_pulses, this);
    } catch (...) { stop(); throw; }
}
FlowmeterComms::~FlowmeterComms() { stop(); }
void FlowmeterComms::stop() noexcept {
    { std::lock_guard<std::mutex> lock(sleep_mutex_); stopping_ = true; }
    sleep_cv_.notify_all();
    if (reader_.joinable()) reader_.join();
    { std::lock_guard<std::mutex> lock(bus_.mtx); producer_done_ = true; }
    bus_.cv.notify_all();
    if (processor_.joinable()) processor_.join();
#ifdef __linux__
    if (event_fd_ >= 0) { ::close(event_fd_); event_fd_ = -1; }
#endif
}
void FlowmeterComms::enqueue(Pulse pulse) {
    { std::lock_guard<std::mutex> lock(bus_.mtx); bus_.deque.push_back(pulse); }
    bus_.cv.notify_one();
}
void FlowmeterComms::read_pulses() noexcept {
    try {
        if (config_.simulate) {
            auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / config_.simulation_hz));
            auto next = std::chrono::steady_clock::now() + period;
            std::unique_lock<std::mutex> lock(sleep_mutex_);
            while (!stopping_) {
                if (sleep_cv_.wait_until(lock, next, [this]{ return stopping_.load(); })) break;
                enqueue({monotonic_ns(), 0});
                // Do not manufacture catch-up bursts after scheduler stalls.
                next = std::chrono::steady_clock::now() + period;
            }
        } else {
#ifdef __linux__
            uint32_t previous_sequence = 0;
            while (!stopping_) {
                pollfd descriptor{event_fd_, POLLIN, 0};
                int ready = ::poll(&descriptor, 1, 100);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0) throw std::system_error(errno, std::generic_category(), "Poll GPIO");
                if (ready == 0) continue;
                if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("GPIO disconnected");
                gpio_v2_line_event events[32];
                auto bytes = ::read(event_fd_, events, sizeof(events));
                if (bytes < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                if (bytes <= 0 || bytes % sizeof(events[0]) != 0) throw std::runtime_error("Invalid GPIO event read");
                for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(events[0]); ++i) {
                    uint32_t missed = events[i].line_seqno - previous_sequence - 1;
                    previous_sequence = events[i].line_seqno;
                    enqueue({events[i].timestamp_ns, missed});
                }
            }
#endif
        }
    } catch (...) { failed_ = true; stopping_ = true; }
    { std::lock_guard<std::mutex> lock(bus_.mtx); producer_done_ = true; }
    bus_.cv.notify_all();
}
void FlowmeterComms::process_pulses() noexcept {
    try {
        for (;;) {
            std::deque<Pulse> batch;
            {
                std::unique_lock<std::mutex> lock(bus_.mtx);
                bus_.cv.wait(lock, [this]{ return producer_done_ || !bus_.deque.empty(); });
                if (bus_.deque.empty() && producer_done_) break;
                batch.swap(bus_.deque);
            }
            std::lock_guard<std::mutex> lock(state_mutex_);
            for (const auto& pulse : batch) {
                ++state_.pulse_count;
                state_.missed_pulses += pulse.missed_before;
                if (previous_ns_ && pulse.timestamp_ns > previous_ns_)
                    state_.pulse_frequency_hz = 1e9 / static_cast<double>(pulse.timestamp_ns - previous_ns_);
                previous_ns_ = pulse.timestamp_ns;
                state_.last_pulse_monotonic_ns = pulse.timestamp_ns;
            }
            if (state_.calibrated) {
                state_.volume_liters = state_.pulse_count / config_.pulses_per_liter;
                state_.flow_liters_per_minute = state_.pulse_frequency_hz * 60 / config_.pulses_per_liter;
            }
        }
    } catch (...) {
        failed_ = true;
        { std::lock_guard<std::mutex> lock(sleep_mutex_); stopping_ = true; }
        sleep_cv_.notify_all();
    }
}
FlowmeterSnapshot FlowmeterComms::snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto result = state_;
    // No edge for at least one second (or three pulse periods): show zero flow.
    double timeout = result.pulse_frequency_hz > 0 ? std::max(1e9, 3e9 / result.pulse_frequency_hz) : 1e9;
    if (!result.last_pulse_monotonic_ns || monotonic_ns() - result.last_pulse_monotonic_ns > timeout) {
        result.pulse_frequency_hz = 0;
        result.flow_liters_per_minute = 0;
    }
    return result;
}
}
