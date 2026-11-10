# Packaged executables and sample

The delivered project ZIP includes these convenience artifacts:

- `bin/mini-drivebrain-macos-arm64`: native Apple Silicon macOS executable for simulation on this machine.
- `bin/mini-drivebrain-pi`: statically linked Linux ARM64 executable for an ARM64 Raspberry Pi 4/5 operating system.
- `samples/simulated-flowmeter.mcap`: a three-second simulated run, 151 snapshots, final received count 53. This is generated data, not a physical sensor recording.
- `SHA256SUMS`: checksums for both executables and the sample.

For a quick local demonstration on this Apple Silicon Mac, from the extracted project directory:

```sh
./bin/mini-drivebrain-macos-arm64 --simulate --hz 20 --recordings ./recordings
```

Connect Foxglove Studio to `ws://localhost:5555` and inspect `/flowmeter`. Stop with Ctrl-C. Calibration is disabled in this command; use the implementation guide if you want to supply a measured coefficient.

For the Pi, follow the deployment guide, substituting `bin/mini-drivebrain-pi` for the build result `build/mini-drivebrain` in the laptop-side transfer command. It must be installed as `/home/nixos/mini-drivebrain` for the supplied service definitions. Verify GPIO chip/line mapping and wiring before starting real input.

The native executable was built with this Mac's installed toolchain; other macOS versions may require rebuilding with `./build_native.sh`. The Linux executable was tested in an ARM64 Ubuntu container using simulation, not on an assigned Pi. Rebuild from source if you change configuration defaults or implementation code. Hardware validation remains required.

The `bin`, `samples` and checksum files are packaging artifacts, rather than upstream source files. The project can always be built from the included source and dependency lockfile.

Updated November 8: both packaged executables now default to `/dev/gpiochip0`, GPIO line offset **12**. Runtime options and an existing service environment file can override these defaults.
