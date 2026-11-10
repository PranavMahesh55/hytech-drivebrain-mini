# Mini Drivebrain: implementation and learning guide

Prepared October 22, 2026. Based on `hytech-racing/drivebrain-mini`, upstream commit `4b5f83d81786264918d2f81fa15a10408f0547bc`.

## 1. What this project accomplishes

This program reads a flowmeter's electrical pulses on a Raspberry Pi, counts them in a worker thread, publishes the current state over Foxglove WebSocket, and saves the same telemetry messages in MCAP files for later playback. It can also generate simulated pulses for development without a sensor.

The assignment is an introduction to the car's multithreaded C++ environment: Linux device access, producer/consumer queues, synchronization, Protobuf, telemetry, logging, and systemd. A working program alone is not the whole onboarding: the original README requires an in-person demonstration and an explanation of your implementation and validation. It also asks participants not to use an LLM to write most of their submission. This implementation was generated with AI at your request; read and understand it, and discuss its use with your onboarding lead before presenting it as completed onboarding work.

**Validation status is recorded in `VALIDATION.md`.** For the delivered executables and sample recording, see `PREBUILT.md`. For the installed and verified laptop viewer, see `VIEWER_SETUP.md`. Source completion does not establish that your actual sensor wiring, board-specific GPIO mapping, or startup service works. Those require a Raspberry Pi.

## 2. Data path and thread ownership

```text
Real GPIO rising/falling edges        Simulated timed pulses
              \                       /
               Reader thread / producer
                         |
       ThreadSafeDeque<Pulse>: deque + mutex + condition variable
                         |
               Processing thread / consumer
                         |
            FlowmeterSnapshot protected by state_mutex_
                         |
              Main thread, every 20 milliseconds
                         |
            drivebrain_msgs::FlowmeterTelemetry
                     /                   \
      Foxglove WebSocket :5555       MCAPLogger queue
                                             |
                                      Logger worker
                                             |
                                  recordings/<time>.mcap
```

There are two sensor workers, the main thread, a logger worker, and the WebSocket library's own internal workers. The main thread owns the sensor object. It is the only caller of `stop()` and owns the lifetime of the singleton helpers. The driver is deliberately noncopyable and is not designed for concurrent calls to `stop()`.

A pulse contains a monotonic timestamp and the number of kernel events missed immediately before that pulse. A snapshot contains the cumulative number of received pulses, last pulse time, estimated frequency, optional calibrated volume and flow, and the cumulative missed-event count.

## 3. File-by-file changes

| File | What changed and why |
|---|---|
| `drivebrain_comms/include/FlowmeterComms.hpp` | Defined configuration, pulse and snapshot structures, the driver API, worker ownership, and mutex/condition-variable state. |
| `drivebrain_comms/src/FlowmeterComms.cpp` | Implemented Linux GPIO v2 edge reading, simulation, queue notifications, a waiting consumer, snapshots, calibration, loss reporting, and shutdown. |
| `proto/drivebrain_msgs.proto` | Added `FlowmeterTelemetry` with sensor values, timestamps, validity and simulation flags. |
| `drivebrain_app/main.cpp` | Added strict command-line parsing, driver construction, 50 Hz publication, final snapshot, signal-safe stop requests, and failure cleanup. |
| `CMakeLists.txt` | Added driver linkage, a generated Protobuf library, tests, a dependency-free driver build, and optional Linux static linking. |
| `drivebrain_comms/CMakeLists.txt` | Linked the standard threading library and enabled compiler warnings. The driver has no telemetry-library dependencies. |
| `drivebrain_core/include/MCAPLogger.hpp` | Added configurable recordings directory, explicit close after draining, and a persistent write-failure indicator. |
| `drivebrain_core/src/MCAPLogger.cpp` | Creates recordings directories, uses timestamped filenames, isolates worker buffers, preserves queue draining, and checks write results. |
| `drivebrain_core/src/FoxgloveServer.cpp` | Uses channel IDs returned by the library rather than assuming consecutive IDs starting at one. |
| `build_script.sh` | Enables static linking explicitly for the Pi build, aligns host/build C++ standards, and limits build parallelism. |
| `conan.lock` | Records dependency versions and recipe revisions resolved for this implementation. |
| `build_native.sh` | Adds a separate native build for simulation, with its own named Conan profile and build folder. |
| `deployment/mini-drivebrain.service` | Provides a Linux systemd unit with configurable chip/line, restart behavior, and a graceful shutdown deadline. |
| `deployment/mini-drivebrain.env.example` | Documents board-specific GPIO configuration. |
| `nixos/configuration.nix` | Updates the existing NixOS service with the same configuration and shutdown behavior. |
| `tests/flowmeter_tests.cpp` | Checks pulse delivery, monotonic counts, calibration, stale values, shutdown, invalid configuration, and repeated construction/destruction. |
| `tests/integration.py` | Connects to the actual WebSocket server, subscribes to telemetry, decodes live Protobuf, stops the process with both signals, and verifies finalized MCAP contents. |
| `.gitignore` | Excludes native/core build directories, recordings, and Python cache files. |

The original README and Pi-image instructions remain available as the original assignment. This guide explains the added implementation.

## 4. Reading GPIO on Linux

The real driver opens a GPIO character device such as `/dev/gpiochip0`, requests one chip-local line using `GPIO_V2_GET_LINE_IOCTL`, and asks for input edge events. Rising edges are the default; `--falling` switches to falling edges. Counting both edges would generally count each pulse twice, so this implementation chooses one edge.

The line request returns a new file descriptor. The chip descriptor is then closed; the request descriptor keeps ownership of the line. The request is made nonblocking. The worker uses `poll()` with a 100 ms timeout, reads batches of up to 32 events, and enqueues their kernel timestamps. The timeout gives shutdown an opportunity to check its stop flag without spinning when no water is flowing. `EINTR` and `EAGAIN` are retried. Disconnection, invalid reads, and other errors cause the worker to mark failure, which the main thread turns into a nonzero process exit.

GPIO v2 is used instead of the older GPIO v1 interface. It exposes sequence numbers, allowing detection of kernel buffer overflows. The code compares successive per-line sequence numbers and reports gaps through `missed_pulses`. It requests a 256-event kernel buffer; the kernel controls the actual buffer implementation. It does not invent replacement pulses for missed events. Consequently the count and volume can underreport if `missed_pulses` is nonzero.

The kernel defaults these timestamps to `CLOCK_MONOTONIC`; Linux `steady_clock` uses the monotonic clock in the target environment. Elapsed-time calculations use these timestamps, while telemetry publication also includes Unix time. Monotonic timestamps must never be interpreted as dates or compared across reboots.

References:

- https://docs.kernel.org/userspace-api/gpio/gpio-v2-get-line-ioctl.html
- https://docs.kernel.org/userspace-api/gpio/gpio-v2-line-event-read.html

### Board mapping and electrical checks

`--line 12` means offset 12 on the selected GPIO chip. It does **not** mean physical header pin 12. Chip enumeration differs across Pi models and operating-system images, especially Pi 5. Run `gpiodetect` and `gpioinfo` on your actual Pi and identify the controller and line routed to the intended header signal. The defaults are examples to verify, not an automatic board detector.

Check your exact flowmeter's datasheet and wiring before connecting it. Pi GPIO signals must stay within 3.3 V logic levels; a sensor powered from 5 V does not automatically have a safe output. Determine whether its output is open collector or push-pull and use the appropriate pull-up or level translation. Use a common ground. Do not infer wire roles solely from colors. This driver does not configure internal bias, debounce, or a sensor power supply.

## 5. Why the queue is thread-safe

`core::ThreadSafeDeque` is a structure containing a deque, mutex and condition variable. Its name does not make arbitrary access safe: callers must follow the locking protocol.

The producer acquires `bus_.mtx`, pushes a pulse, releases the mutex, and notifies `bus_.cv`. The consumer acquires the same mutex and calls:

```cpp
bus_.cv.wait(lock, [this]{ return producer_done_ || !bus_.deque.empty(); });
```

The predicate handles spurious wakeups and notifications that arrive before the consumer starts waiting. `wait` releases the mutex while sleeping and reacquires it before returning. The consumer swaps the shared deque into a local batch while holding the lock. It then releases that lock before calculating the new state. That keeps the producer from waiting on processing work.

`producer_done_` is protected by the queue mutex. Only a completed producer permits the consumer to exit with an empty queue. A stop request alone is insufficient: the producer might still be about to enqueue its final batch.

The published snapshot has a separate mutex. The consumer writes all fields while holding `state_mutex_`; the main thread copies all fields under that same mutex. A snapshot therefore does not combine a new pulse count with old calibration values. The main thread logs the copy without holding the state lock.

## 6. Simulation and calculations

Simulation uses the **same enqueue and consumer path** as real GPIO. It replaces only the reader's physical input. An interruptible timed wait generates one pulse per requested period. Scheduler delays reduce the actual generated frequency; the simulator intentionally avoids manufacturing catch-up bursts.

Frequency is estimated from the most recent pair of received edges:

```text
frequency_hz = 1,000,000,000 / elapsed_nanoseconds
volume_liters = received_pulse_count / pulses_per_liter
flow_liters_per_minute = frequency_hz * 60 / pulses_per_liter
```

The first pulse has no previous interval, so its frequency remains zero. With no new pulse for the larger of one second or three estimated periods, the snapshot reports zero frequency and flow. Count and volume remain cumulative.

Calibration defaults to disabled. Until you supply a positive `--pulses-per-liter`, `calibrated` is false and volume/flow remain zero. Zero does not then mean a measured zero-volume result. Frequency and count still work.

For a real calibration, pass a known measured volume through the sensor, record the number of pulses, and calculate `pulses / measured_liters`. Repeat at representative flow rates to assess consistency. The guide's example value of 100 is for simulation, not a specification for the supplied flowmeter. Raw interval frequency can be noisy; filtering is an optional future improvement.

## 7. Protobuf and telemetry

`FlowmeterTelemetry` fields:

| Field | Meaning |
|---|---|
| `timestamp_unix_ns` | System-clock timestamp of this snapshot, in nanoseconds since Unix epoch. |
| `pulse_count` | Received and processed pulses since this process started. |
| `last_pulse_monotonic_ns` | Kernel/simulator time of the latest pulse; not wall-clock time. |
| `pulse_frequency_hz` | Latest interval estimate, zeroed after the idle timeout. |
| `calibrated` | Whether a positive calibration coefficient was supplied. |
| `volume_liters` | Cumulative received-pulse volume; meaningful only when calibrated. |
| `flow_liters_per_minute` | Interval-based flow estimate; meaningful only when calibrated. |
| `missed_pulses` | Detected missing kernel edge events, not automatically added to volume. |
| `simulated` | Distinguishes generated input from a physical sensor run. |

CMake runs `protoc` and builds the generated C++ files into `drivebrain_msgs`. Both existing helper classes find the generated descriptor and embed the schema so consumers can decode messages without a separate `.proto` file.

The main thread publishes snapshots at 50 Hz on `/flowmeter`. Pulse capture runs independently, so the reader can capture multiple pulses between publications. The saved file records snapshots, not one message per raw edge. Live and disk receive the same message object, including its measurement timestamp; their helper-generated transport timestamps can differ slightly. Their received message counts can also differ if the viewer connects late.

## 8. Shutdown, ownership, and error handling

The signal handler only sets a `volatile std::sig_atomic_t` flag. It does not log, take a mutex, destroy an object, or join a thread inside the handler. The main loop notices the flag on its next iteration.

Normal shutdown proceeds in this order:

1. Request the reader to stop and wake its simulated timed wait.
2. Join the reader. A GPIO read loop wakes within its poll timeout under normal scheduling.
3. Mark the producer complete under the queue mutex and notify the consumer.
4. Let the consumer finish queued pulses and join it.
5. Take and publish a final snapshot with the fully drained count.
6. Stop the WebSocket server.
7. Stop the logger, drain its own message queue, join its writer thread, and explicitly close the MCAP writer.

Stopping an idle reader is important: a thread stuck forever in a blocking read or a long sleep would prevent `systemctl stop` from completing. The simulator uses a condition variable; changes to its stop predicate use the same sleep mutex to avoid lost notifications.

The driver's constructor acquires GPIO before starting threads. If thread creation fails, cleanup stops and joins any worker already created and closes the request descriptor. Runtime exceptions in main unwind the local driver before helper destruction. Worker failures are reported via atomics. MCAP write errors are logged and cause an unsuccessful process result.

`SIGKILL`, abrupt power loss, disk failure, and system shutdown deadlines can still interrupt file completion. A damaged recording may need MCAP recovery. A normally stopped run should have its summary/footer and should not need recovery merely to open it.

## 9. Building and running

### Raspberry Pi deployment build

Install Docker and start its daemon. From the repository root:

```sh
docker compose run --rm dev ./build_script.sh
```

The result is `build/mini-drivebrain`. The original Docker environment uses Ubuntu 22.04 and the aarch64 GCC 11 toolchain, while Conan supplies telemetry dependencies. Static linking is enabled for this target to bundle the application dependencies. The linker warns that optional glibc resolver/dynamic-loader paths may still rely on runtime components; the validation report records this, and the actual Pi image still needs a deployment test. The first build compiles dependencies and can take substantially longer than later builds. Both build scripts default to two parallel jobs to limit memory usage. Set `BUILD_JOBS` to adjust this (for Docker, pass it with `docker compose run -e BUILD_JOBS=4 ...`).

### Native simulation build

Install a C++17 compiler, CMake and Conan 2. Ensure both commands are on your PATH, then run:

```sh
./build_native.sh
./build-native/mini-drivebrain --simulate --hz 20 --recordings ./recordings
```

Use a finite run for a quick check:

```sh
./build-native/mini-drivebrain --simulate --hz 100 --pulses-per-liter 100 --duration 5 --recordings ./recordings
```

A native macOS executable cannot run on the Pi. Keep `build-native` and the Pi's `build` separate.

### Driver-only build

This requires only CMake and a C++17 compiler, without Conan or telemetry libraries:

```sh
cmake -S . -B build-core -DDRIVEBRAIN_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 4
ctest --test-dir build-core --output-on-failure
```

### All runtime options

| Option | Default / behavior |
|---|---|
| `--simulate` | Generate pulses; default is real GPIO. |
| `--hz N` | Simulation target frequency, default 20; must be finite, positive and no more than 1,000,000. |
| `--chip PATH` | GPIO character device, default `/dev/gpiochip0`. |
| `--line N` | Chip-local unsigned offset, default 12. |
| `--falling` | Count falling edges instead of rising edges. |
| `--pulses-per-liter N` | Enable volume/flow conversion with a positive coefficient. |
| `--recordings DIR` | MCAP directory; defaults to `/home/nixos/recordings`. Created if needed. |
| `--duration N` | Stop after a positive number of seconds; otherwise run until signaled. |
| `--help` | Print usage. |

Unknown options, malformed numbers, nonfinite values, negative offsets and invalid durations fail instead of silently using defaults.

## 10. Deploying to the assigned Pi

Connect to its `mini-drivebrain` Wi-Fi. The provided image documents `192.168.203.67` as the Pi address and `nixos` as the SSH user. Confirm your particular Pi's assigned network/address.

Run these **on your laptop**, not from inside the Pi SSH session:

```sh
ssh nixos@192.168.203.67 'sudo systemctl stop mini-drivebrain.service'
scp build/mini-drivebrain nixos@192.168.203.67:/home/nixos/mini-drivebrain.new
ssh nixos@192.168.203.67 'chmod +x /home/nixos/mini-drivebrain.new && mv /home/nixos/mini-drivebrain.new /home/nixos/mini-drivebrain'
```

Stop the existing service before replacing its executable, as the assignment requests. Staging under a different filename avoids overwriting a partially transferred executable. Keep one writer process per recordings directory.

Before starting, inspect GPIO mapping over SSH:

```sh
gpiodetect
gpioinfo
```

Create `/etc/mini-drivebrain.env` with your verified chip and offset, using `deployment/mini-drivebrain.env.example` as a template. Then:

```sh
sudo systemctl start mini-drivebrain.service
sudo systemctl status mini-drivebrain.service
sudo journalctl -u mini-drivebrain.service -f
```

### NixOS versus conventional Linux service installation

The supplied NixOS image already defines `mini-drivebrain.service`. The updated `nixos/configuration.nix` adds the environment-file configuration. For an existing image, have the Pi owner apply the updated NixOS configuration through its normal rebuild process, or add a systemd override with the verified command. The environment file only takes effect once the service definition references it. Do not assume copying source configuration changes a running Pi.

For a conventional systemd Linux image, install the standalone unit:

```sh
sudo install -m 644 deployment/mini-drivebrain.service /etc/systemd/system/mini-drivebrain.service
sudo install -m 644 deployment/mini-drivebrain.env.example /etc/mini-drivebrain.env
# Edit the environment file to match your board first.
sudo systemctl daemon-reload
sudo systemctl enable --now mini-drivebrain.service
```

The standalone unit and teaching NixOS service run as root for access to GPIO. A restricted deployment should configure a dedicated user and device permissions. The shipped service does not set a calibration coefficient; to enable calibrated units or falling-edge input, amend `ExecStart` in the appropriate unit/override or NixOS definition.

## 11. Viewing live data and replaying recordings

In Foxglove Studio, open a Foxglove WebSocket connection to `ws://192.168.203.67:5555` (or `ws://localhost:5555` for native simulation). Add a Raw Messages panel for `/flowmeter`, then plots for pulse count, frequency, and calibrated flow if enabled. Check `simulated` before treating the run as a hardware demonstration.

Stop the program cleanly and copy a specific `.mcap` from `/home/nixos/recordings` to your laptop. Open it in Foxglove and inspect `/flowmeter` again. Its final pulse count should match the final count printed in the process log. Use `mcap recover` only if the recording was interrupted or the viewer reports corruption; the upstream README mentions this tool for pulled recordings.

Live telemetry binds to `0.0.0.0:5555` through the provided helper and has no authentication/TLS. This matches the isolated teaching-network design. Do not expose it publicly without adding network controls.

## 12. Tests and what they prove

The C++ driver test uses generated pulses and checks:

- pulses reach the consumer and counts never go backwards;
- calibrated volume equals count divided by coefficient;
- frequency is positive and the flow conversion is consistent;
- stopped counts stay fixed and repeated `stop()` is safe;
- stale frequency becomes zero;
- stopping with the next simulated pulse 100 seconds away does not wait 100 seconds;
- invalid rate inputs are rejected;
- 50 immediate construction/destruction cycles complete.

The Python integration check launches the actual executable and tests both SIGINT and SIGTERM. It connects with the Foxglove subprotocol, waits for `/flowmeter` advertisement, subscribes, receives a binary frame, and decodes its Protobuf data. It then reads the completed MCAP using its embedded schema, checks a summary exists, validates counts and calibrated values, and compares the final count against the process log.

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
.venv/bin/python tests/integration.py build-native/mini-drivebrain
```

The integration test also checks that an invalid recordings path and an unavailable GPIO device cause a prompt unsuccessful exit. It needs port 5555 free. Simulation tests do not establish the electrical signal quality, actual chip permissions, kernel timing under load, or accuracy of calibration. See `VALIDATION.md` for actual results from this session.

## 13. Hardware acceptance checklist

- Confirm exact flowmeter power, output type, voltage translation and grounding.
- Identify chip and offset with Pi GPIO tools; ensure the line is not already in use.
- Supply a known number of test pulses and compare the received count. Check `missed_pulses` stays zero.
- Connect the real flowmeter, create flow, and verify live count/frequency.
- Stop flow and verify the estimate eventually returns to zero.
- If using volume units, measure calibration against a known volume and document the result.
- Press Ctrl-C during foreground operation; verify prompt exit and valid MCAP playback.
- Start the service, run with input, then `systemctl stop`; verify it becomes inactive and the recording opens.
- Start and reboot the Pi; verify the enabled service starts and produces telemetry without manual login.
- Replay the hardware run in Foxglove and compare its final count with the journal.

## 14. Boundaries and useful follow-up work

The application queue and MCAP queue are unbounded. This is reasonable for a low-rate onboarding sensor but can grow under sustained overload. A production system should add limits, explicit drop metrics, and a tested policy for overload. Kernel sequence gaps detect loss before userspace; they do not measure electrical pulses that never produced an interrupt.

No software debounce/filtering is enabled. Noise can look like real pulses, while overly aggressive filtering can reject valid pulses. Choose signal conditioning from actual measurements. The interval estimate is not a rolling average and low-frequency stop detection intentionally waits longer.

A runtime worker error is propagated as a general failure; detailed operating-system errors during initial GPIO acquisition are printed directly. The logger checks write results and marks worker exceptions as failures, but does not implement recovery from disk exhaustion or retry failed writes. File names use wall-clock nanoseconds and an existence check; there is no multi-process locking, rotation or disk quota. The service should be the sole writer for its directory.

The provided network image and WebSocket helper are for a teaching environment. This project does not redesign authentication, sensor protocols, the Pi image, or the car's production software.

## 15. Questions you should be able to answer

1. Why use two sensor threads and a queue rather than polling a shared integer?
2. Which mutex protects each shared value? Why does a condition variable need a predicate?
3. What happens if a notification comes before the consumer waits?
4. Why process a local batch after releasing the queue lock?
5. How do GPIO timestamps differ from Unix timestamps?
6. How are frequency, volume and liters/minute calculated? What does uncalibrated mean?
7. Why does the consumer wait for producer completion before exiting?
8. What happens if Ctrl-C arrives when there are no pulses?
9. How do Protobuf descriptors make an MCAP self-describing?
10. What does the simulator verify, and what must still be checked with hardware?
11. Why can missed kernel events make reported volume too small?
12. How do you distinguish a native laptop binary from the Pi deployment binary?

Read the code while tracing one pulse from device read through to a saved snapshot. Then trace shutdown while the sensor is idle. Those two exercises cover the main ideas behind the onboarding.
