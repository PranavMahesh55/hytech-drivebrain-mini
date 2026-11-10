# Laptop Foxglove viewer setup

Prepared November 3, 2026.

Foxglove 3.5.0 for Apple Silicon was installed at `/Applications/Foxglove.app` from the current download link on the official Foxglove download page. The app signature passed `codesign --verify --deep --strict`.

The app requires a Foxglove account before its local-file and live-connection commands become available. The user completed sign-in. Configuration and verification in the actual desktop viewer are now complete, in addition to the earlier automated protocol tests.

## Start the local simulation

Double-click `Start Flowmeter Simulator.command` in the project folder. Leave its Terminal window running. Press Control-C in that Terminal to stop it and finalize its recording. If port 5555 is already in use, stop the existing simulator instead of launching another copy.

## Verified configuration

The saved personal layout is **HyTech Flowmeter**. It contains a Raw Messages panel for `/flowmeter`, a pulse-count plot, and a pulse-frequency plot. The layout was saved explicitly and exported to `viewer-verification/HyTech Flowmeter.json` for local backup/import.

Live verification: Foxglove connected to `ws://localhost:5555`, decoded `drivebrain_msgs.FlowmeterTelemetry`, and displayed increasing pulse counts and nonzero frequency. `simulated` was true and `missed_pulses` was zero.

Playback verification: the simulator was stopped cleanly, and its finalized MCAP was opened directly in Foxglove. Playback advanced the raw pulse count from 1 to 11 to 146; both plots rendered the recording. `viewer-verification/playback.png` captures the verified viewer. The simulator is now stopped, and Foxglove is left open on the paused recording.

The integer plot warning about conversion to 64-bit floating point is Foxglove's standard warning for uint64 values; the small pulse counts used here are exactly representable. The Raw Messages panel retains the original integer values.

## Repeating the viewer checks

1. Open Foxglove, choose Open connection, select Foxglove WebSocket, and use `ws://localhost:5555`.
2. Add a Raw Messages panel and select `/flowmeter`. Confirm `simulated` is true and `pulse_count` increases.
3. Add plots for `/flowmeter.pulse_count` and `/flowmeter.pulse_frequency_hz`.
4. Stop the simulator cleanly. Open the finalized `.mcap` in `viewer-verification/recordings` in Foxglove and play it back. The same count/frequency fields should appear.
5. For the team's Pi, change the connection to `ws://192.168.203.67:5555` (or the actual assigned Pi address), with the Pi service running and your laptop connected to its network.

The simulator does not validate real GPIO wiring or the Pi's runtime configuration. Calibration is optional and is not required by the assignment README.

## Recorded run details

The GUI-verified file is `viewer-verification/recordings/1793762609558689000.mcap`. It contains 11,850 snapshots and a final received pulse count of 4,436. These are simulated data. Opening this finalized file did not require `mcap recover`.
