# Validation record

Date: October 22, 2026. Upstream baseline: `4b5f83d81786264918d2f81fa15a10408f0547bc`.

## Results

| Check | Result | Evidence / scope |
|---|---|---|
| macOS driver-only C++ build | PASS | AppleClang 21, C++17, Debug. |
| macOS driver tests | PASS | Pulse delivery, monotonic snapshots, calibration, idle decay, repeated stop, long-period wakeup, invalid input and 50 lifecycle cycles. |
| Linux driver-only C++ build | PASS | Ubuntu 22.04 ARM64 / GCC 11; compiles the real Linux GPIO v2 branch as well as simulation. |
| Linux driver tests | PASS | Same driver test suite; no GPIO device attached. |
| Full macOS application build | PASS | Protobuf generation, sensor driver, Foxglove helper and MCAP logger linked successfully. |
| Native SIGINT end-to-end test | PASS | Live `/flowmeter` subscription decoded; 24 MCAP snapshots; final received-pulse count 33 matches process log. |
| Native SIGTERM end-to-end test | PASS | Live `/flowmeter` subscription decoded; 24 MCAP snapshots; final received-pulse count 32 matches process log. |
| Invalid command-line inputs | PASS | NaN rate, negative line, zero duration and unknown option rejected. |
| Startup failure cleanup | PASS | Invalid recordings directory and unavailable GPIO fail promptly without hanging helper threads. |
| Sample finite-duration recording | PASS | Three seconds, simulated 20 Hz target, calibration 100 pulses/liter; 151 snapshots, final count 53, finalized MCAP readable with embedded schema. Scheduler delays reduce generated frequency. |
| Pi deployment build | PASS | Final `build_script.sh` completed in Ubuntu 22.04 ARM64 / GCC 11. `file` reports a statically linked ARM aarch64 ELF executable; `readelf -d` reports no dynamic section. |
| Pi executable simulated integration | PASS | Actual packaged Linux binary: SIGINT produced 25 snapshots / final count 35; SIGTERM produced 25 snapshots / final count 36. Both decoded live and replayed from finalized MCAP, with counts matching process logs. Invalid arguments, recordings path and unavailable GPIO cleanup also passed. |
| Standalone systemd unit syntax | PASS | `systemd-analyze verify` returned success with the executable staged at the declared path inside the validation container. No live systemd service/boot was exercised. |
| Source formatting and script syntax | PASS | `git diff --check`, shell syntax parsing, Python compilation. |
| Physical flowmeter capture | NOT RUN | Requires the actual sensor and Pi. |
| Real chip/line mapping and electrical levels | NOT RUN | Board- and sensor-specific. |
| Service start/stop on an assigned Pi | NOT RUN | Syntax validation alone does not verify the Pi's runtime configuration. |
| Automatic startup after Pi reboot | NOT RUN | Requires deployed hardware. |
| Physical volume calibration | NOT RUN | Requires measured water volume and the actual flowmeter. |

The results are simulation results unless explicitly stated otherwise. No hardware accuracy or completion of the in-person onboarding demonstration is claimed.

## Dependencies and build environment

C++ dependencies come from the original `conanfile.txt`: Foxglove WebSocket 1.4.0, Protobuf 5.29.3, spdlog 1.13.0 and MCAP 2.0.2. `conan.lock` records the resolved dependency recipe revisions, including their transitive dependencies. Both the native and Linux builds use C++17. The Pi build requests a static Linux ARM64 executable.

The Python integration tests use MCAP 1.6.0, mcap-protobuf-support 0.6.0 and websockets 15.0.1, pinned in `tests/requirements.txt`. This WebSocket version supports Ubuntu 22.04's Python 3.10 as well as the local Python 3.14 test environment.

The static link produced glibc warnings about optional `dlopen`, `getaddrinfo` and `gethostbyname` paths, inherited through OpenSSL/Asio. The exercised numeric-address, non-TLS WebSocket path works in the Linux container. Static ELF metadata alone does not prove all optional resolver/provider behavior will work on a different image; test the actual NixOS Pi before accepting deployment.

## What was checked end to end

The test launches a separate application process with simulated pulses and a temporary recordings directory. It connects using `foxglove.websocket.v1`, waits for a channel with topic `/flowmeter`, verifies its advertised Protobuf schema, subscribes, and receives a binary message frame. It then sends SIGINT or SIGTERM to the process and checks successful exit within five seconds.

The test reads the generated MCAP with a separate Python library, checks that a summary/footer is present, decodes telemetry from the file's embedded schema, verifies nondecreasing counts and calibrated volumes, and compares the final recorded count to the process's final count. It also decodes the captured WebSocket payload using the same message schema. This verifies the complete simulated data path across process, socket and file boundaries.

The October 22 automated tests validated the protocol and recording contents. On November 3, Foxglove 3.5.0 was installed and the actual desktop viewer was also verified with live simulated telemetry and MCAP playback. See `VIEWER_SETUP.md` and `viewer-verification/playback.png`. The physical hardware acceptance checklist still requires a Pi and flowmeter.

## Issues encountered and resolved

- Docker was initially unavailable. Driver-only checks and a native simulation build provided useful verification while the user started Docker. Linux checks were then added.
- The initial native dependency build could not find CMake in the child process PATH. Adding the isolated tooling directory to PATH resolved it; CMake must be on PATH when using Conan.
- The upstream sensor header/source and message file were placeholders; they were implemented rather than treating the original main loop as a completed application.
- The MCAP helper depended on implicit writer destruction for file finalization. It now explicitly closes the writer after draining and joining its worker.
- Foxglove channel registration assumed IDs began at one. It now records the actual IDs returned by `addChannels`.
- Snapshot publication needed a shared-state protocol, and the sleeping producer needed a stop predicate protected by its wait mutex. Both are synchronized explicitly.
- The newest Python WebSocket package available locally requires Python 3.11. Tests were pinned to a version that also supports the Docker image's Python 3.10, and the native integration test was rerun with that version.
- The ARM Docker build initially selected C++17 for target dependencies and GNU C++17 for build tools, causing a duplicate Protobuf build. The script now explicitly uses C++17 for build tools too, allowing the same package to be reused when host and target otherwise match.
- The minimal build image lacks `systemd-analyze`; systemd tools were installed in a temporary validation container, and unit syntax was verified there. The container was removed after testing.

## Repeating the checks

From the project root, with the documented prerequisites installed:

```sh
cmake -S . -B build-core -DDRIVEBRAIN_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure
./build_native.sh
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
.venv/bin/python tests/integration.py build-native/mini-drivebrain
docker compose run --rm dev ./build_script.sh
```

Keep port 5555 free before running the integration test. For the physical test, follow the implementation guide's deployment and hardware acceptance sections.

## November 3 desktop viewer verification

PASS: Installed and signature-verified Foxglove 3.5.0 for Apple Silicon. After user sign-in, configured and saved the **HyTech Flowmeter** layout, verified live decoded messages and both plots, opened a finalized recording and verified playback, and exported the layout JSON. The simulator is stopped and the viewer is left open on the recording.

## November 8 GPIO input configuration update

Default input changed from `/dev/gpiochip0` line 17 to `/dev/gpiochip0` **GPIO line offset 12**. Updated the driver default, command-line help, standalone service, environment template, NixOS service defaults, and documentation. Rebuilt both packaged executables.

PASS: Native build and driver tests, Linux ARM64 static build and driver tests, and native/Linux end-to-end simulation tests (live WebSocket decoding, finalized MCAP replay, SIGINT and SIGTERM shutdown, and startup failure cleanup). The Linux signal tests each produced 25 snapshots with final pulse count 40. Both executables advertise `--line 12` in their help. See `gpio12-native-tests.log` and `gpio12-pi-tests.log`.

This configures a chip-local GPIO offset, not physical header pin 12. An existing `/etc/mini-drivebrain.env` can override the service defaults; set `GPIO_CHIP=/dev/gpiochip0` and `GPIO_LINE=12` there if that file is present on the Pi. Use `--chip /dev/gpiochip0 --line 12` for an explicit foreground invocation. The actual Pi was not accessed or changed, and physical GPIO capture is still untested.
