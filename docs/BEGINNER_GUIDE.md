# Mini Drivebrain, from the beginning

This guide explains the project you have, rather than assuming you already know embedded systems or multithreaded C++. Read it once for the overall picture. Then use the exercises to connect the explanation to the actual code and viewer.

## 1. The project in one sentence

Build a C++ program that runs on a Raspberry Pi, receives electrical pulses from a flowmeter, processes them safely, displays current measurements in Foxglove, and records the measurements for later replay.

The learning goal is to practice the same patterns used in the car's larger Drivebrain software: receive sensor input, coordinate work across threads, publish useful data, and preserve a recording.

You are not being asked to design the whole car computer. You are working on one small sensor-data pipeline.

## 2. The physical setup

A Raspberry Pi is a small computer. It runs an operating system and can run the executable compiled from your C++ source. Your laptop is the development computer and viewer; the Pi is the computer attached to the sensor.

A flowmeter produces electrical pulses as liquid passes through it. Think of them as repeated electrical ticks. The exact relationship between pulses and liquid volume depends on the actual sensor and its calibration.

GPIO means General Purpose Input/Output. Here, a GPIO input detects a digital signal. An edge is a transition: rising means low to high, and falling means high to low. Your implementation counts one selected edge per pulse, rising by default. Counting both edges could count each physical pulse twice.

The signal reaches a GPIO controller exposed by Linux as a device such as `/dev/gpiochip0`. A line number is an offset within that controller. **Line 12 is not physical header pin 12.** The correct chip and line must be checked on the actual Pi.

Electrical compatibility is also a hardware check. Do not guess the sensor's output voltage, output type, wire colors, or required pull-up. Verify the sensor and Pi-safe signal levels with the team's hardware setup before connecting them.

## 3. What runs where

| Component | Where it runs | Its job |
|---|---|---|
| Sensor reader, processing, main loop, and logging | Pi for real input; Mac for simulation | Your C++ application |
| Foxglove server helper | Inside that application | Makes live messages available on port 5555 |
| Foxglove desktop app | Your laptop | Displays live data and replays local recordings |
| MCAP file | Initially on the computer running the application | Stores timestamped telemetry messages |
| systemd service | Pi's Linux operating system | Starts and supervises the application |
| Docker, Conan, CMake and compiler | Development/build environment | Turn source code and dependencies into an executable |

Foxglove does not read the Pi's GPIO for you. The C++ application does that. In this project, the C++ logger also writes the recording; the viewer is not required to remain connected for the program to record.

## 4. The minimum programming vocabulary

A **variable** holds a value. `pulse_count` holds a count. A **type** defines the kind of value: `bool` is true/false, `uint64_t` is a nonnegative 64-bit integer, and `double` holds fractional numbers.

A **function** is a named piece of work. `snapshot()` returns the latest protected copy of the sensor state. `main()` is where the application starts.

A **loop** repeats work. The main loop repeatedly takes a snapshot and publishes it until shutdown is requested.

A **struct** groups related values. `Pulse` contains an event timestamp and missing-event information. `FlowmeterSnapshot` groups the latest count, frequency and other fields.

A **class** groups state with functions that operate on that state. `FlowmeterComms` owns the sensor reader, queue, processor and latest state. An **object** is a particular instance of a class created when the program runs.

A **constructor** initializes an object. The `FlowmeterComms` constructor validates configuration, acquires GPIO if needed, and starts its workers. A **destructor** cleans up when the object leaves scope. Its destructor calls `stop()`.

A `.hpp` file declares the interface and data structures; a `.cpp` file contains the implementations. You read what the class provides in the header, and how it works in the source file.

## 5. Follow one pulse

1. The flowmeter produces a selected electrical edge.
2. Linux captures an edge event and its time.
3. The reader worker obtains the event and makes a `Pulse`.
4. The reader adds the pulse to the shared deque while holding its mutex, releases that lock, and notifies the condition variable.
5. The processing worker wakes if necessary, takes queued pulses into a local batch while holding the queue lock, then releases that lock.
6. The processor updates the cumulative count and timing estimate under a different mutex protecting the latest state.
7. The main loop asks for a protected copy of that state.
8. It fills a Protobuf telemetry message and passes the same message to the live server and MCAP logger.
9. Foxglove receives the live message over the network; the logger's worker saves the queued message to disk.

One received pulse does not necessarily create exactly one saved message. Our program publishes snapshots of the current state at a separate rate.

## 6. Threads: separate jobs within one program

A thread is a separate sequence of execution within the same running program. Threads can make progress independently, and they share the program's memory. The operating system schedules them; they do not necessarily all run at exactly the same instant.

The main application thread coordinates startup, publication and shutdown. There are two sensor workers: a producer that receives pulses and a consumer that processes them. The logger has its own worker, and the WebSocket library manages internal work as well. This is not an application with exactly three total threads.

Separating jobs allows the sensor reader to keep receiving events while the rest of the program processes, publishes or writes data. That improves responsiveness, but it does not guarantee that unlimited event rates can be handled without loss.

## 7. The deque, producer and consumer

A deque is a double-ended queue. Here, it behaves like a waiting line: events enter the shared queue, and the consumer takes batches out for processing.

The reader is the **producer** because it produces pulse events. The processor is the **consumer** because it takes those events and updates the sensor state.

The queue buffers short timing differences. If several pulses arrive before the consumer is scheduled, they can wait in the queue rather than overwriting one shared latest-event variable.

The supplied `core::ThreadSafeDeque` contains three things:

```text
deque: the events
mutex: protection for shared access
condition variable: a way for a worker to sleep and be notified
```

**The structure does not automatically lock its deque.** Its callers must use the mutex correctly. The class name is not a guarantee that arbitrary accesses are safe.

## 8. Mutexes and consistent snapshots

A mutex is a lock for a shared resource. A thread acquires it before accessing the protected state, and releases it afterward. Other threads using the same mutex must wait until that access is finished.

Our implementation has separate locks for separate shared resources. `bus_.mtx` protects the event queue and producer-completion flag. `state_mutex_` protects the latest processed measurement. The simulator also has a sleep mutex for its interruptible wait.

Without synchronization, one thread writing memory while another reads it can create a C++ data race, which is undefined behavior. Even thinking only about the intended values, the reader might see fields from different updates rather than one consistent measurement.

`snapshot()` locks the measurement state, copies it, and releases the lock. The main thread then works with its own copy. It does not hold the state lock while serializing, networking or logging.

A useful analogy is a shared notebook: the writer completes an entry while holding the key; the reader takes a photocopy under that same key, then reads the photocopy without blocking new entries.

## 9. Condition variables and why the consumer waits

A busy loop repeatedly asks whether anything has arrived. When there are no pulses, that wastes CPU.

A condition variable allows the consumer to sleep while waiting for work. The producer adds an event and notifies it. The consumer's actual test is whether the queue has data or the producer has finished.

Simplified form of our wait:

```cpp
cv.wait(lock, [&] { return producer_done || !queue.empty(); });
```

The predicate is the expression deciding whether the worker may proceed. `wait` releases the queue mutex while sleeping, then reacquires it before checking and accessing the queue. Keeping the lock while sleeping would prevent the producer from filling the queue.

A notification is not a saved event and is not proof that data is present. The predicate handles notifications arriving before a wait and spurious wakeups. The queued data is what preserves the events.

The consumer swaps the shared deque into a local batch while holding the queue lock. It releases that lock before processing. This makes the protected queue operation short, so receiving new pulses is not blocked by the calculations.

## 10. Atomics, ownership and lifetime

An atomic supports specified operations that can safely be shared across threads without an ordinary mutex. We use atomic booleans for stop/failure flags.

An atomic is useful for a small flag, but separate atomic fields do not automatically make a whole multi-field measurement a consistent snapshot. That is why the measurement uses a mutex.

The signal handler uses a separate signal-safe flag (`sig_atomic_t`) to request shutdown. Signal handling has stricter rules than ordinary threaded code; it should not perform logging, acquire mutexes or join workers inside the handler.

The driver owns its workers and GPIO request. It cannot be copied. Copying an owner of running threads and device handles would make it unclear which object must stop and release them. Automatic cleanup on scope exit is an example of the C++ ownership technique often called RAII: tie a resource's lifetime to an object.

## 11. Counts, frequency and optional calibrated values

**Pulse count** is the total number of received pulses processed since this application started. It stays at its accumulated value when input stops, and starts again at zero when a new process starts.

**Frequency** is how quickly pulses arrive, measured in hertz (Hz). One hertz means one pulse per second. Our estimate uses the interval between the latest two received pulses:

```text
frequency = 1 / seconds between pulses
```

If the interval is 0.05 seconds, the estimate is 20 Hz. One pulse by itself is insufficient to establish an interval. The estimate may fluctuate with event timing and simulation scheduling.

The snapshot reports zero frequency after input has been inactive longer than the larger of one second or three estimated pulse periods. It retains the cumulative pulse count.

**Calibration is optional for this assignment.** If you independently determine a coefficient in pulses per liter, the program can calculate volume and flow rate. Without it, `calibrated` is false and the volume/flow fields are placeholders at zero. Those fields should not be interpreted as calibrated measurements.

A **timestamp** records when something occurred. The program uses a monotonic clock, like a stopwatch, for pulse timing; it uses a wall clock, like a calendar clock, for publication time. A nanosecond is one billionth of a second. The large Unix-time field is not a pulse count, and the monotonic field is not a date.

## 12. Capture rate and publication rate are different

The simulator targets 20 pulses per second by default. The main loop targets one publication every 20 milliseconds, or 50 snapshots per second. Both are subject to scheduling delays.

At 20 pulses/second and 50 publications/second, repeated snapshots with the same count are normal. At an input of 100 pulses/second, a 50 Hz snapshot stream can show the count increasing by roughly two between publications.

Those jumps do not necessarily mean missed pulses. Capture and processing run independently of publication. A snapshot summarizes all pulses processed up to that moment. The MCAP stores these snapshots, not a separate record of every raw GPIO edge.

`missed_pulses` is a separate field that tracks detected gaps in the kernel's event sequence numbers. It does not detect every possible electrical problem, and it does not add invented pulses to the count.

## 13. Protobuf, topics, WebSocket, Foxglove and MCAP

A **Protobuf message** defines named fields and their types. `proto/drivebrain_msgs.proto` defines `FlowmeterTelemetry`. It is a data contract, so the sender and receiver agree on what each value means.

The readable content of a telemetry message might be:

```text
pulse_count: 42
pulse_frequency_hz: 20.0
simulated: true
```

On the wire and in the recording, Protobuf is encoded as binary data. The helper classes include a schema describing how to decode that binary data.

A **topic** is a label grouping a stream of messages. `/flowmeter` is our topic. Its slash does not make it a file-system directory.

A **WebSocket** is a persistent network connection for sending live messages. Our program hosts a Foxglove-protocol WebSocket server on port 5555. The Foxglove desktop app connects as a client. The server helper and the desktop viewer are different components.

`ws://localhost:5555` means connect to port 5555 on the same computer as the viewer. With the program on the Pi and viewer on your laptop, localhost would still mean your laptop. Use the Pi's reachable address, such as the README's `ws://192.168.203.67:5555`, instead.

**Foxglove** shows data. The Raw Messages panel displays the fields. The pulse-count plot rises as pulses accumulate. The frequency plot shows the current interval estimate. A layout remembers which panels and fields to display; it is not a recording of sensor data.

An **MCAP** is the recording file. It stores timestamped messages, channel information and schemas so the data can be replayed. The C++ program can record even while the viewer is disconnected. The normal Pi recordings directory is `/home/nixos/recordings`.

The original README tells you to run `mcap recover` on pulled recordings before viewing them. Our cleanly finalized simulated recordings opened directly. Recovery can be relevant for an incomplete or damaged recording; it does not replace proper shutdown.

## 14. What the main loop does

`main.cpp` reads options, installs signal handlers, initializes the logger and live server, creates the driver, and starts its repeated publication loop.

In plain language:

```text
while shutdown has not been requested:
    check worker health
    take a consistent copy of the latest processed state
    build one telemetry message
    send it to the MCAP logger and Foxglove helper
    wait until the next scheduled publication
```

The main thread does not receive each electrical pulse directly. The reader does that. The main thread publishes the state produced by the consumer.

An error such as an unavailable GPIO device or unusable recordings path should fail visibly rather than pretending that healthy hardware data is available. The tests check cleanup for these startup failures too.

## 15. Why shutdown is part of the assignment

Ctrl-C sends SIGINT. A normal service stop sends SIGTERM. Your program should treat them as requests to finish safely.

Its shutdown sequence is:

1. The signal handler sets the interruption flag.
2. Main notices it and requests the reader to stop.
3. The simulated wait is notified; the real GPIO polling loop has a bounded wait so it can notice stop.
4. Main joins the reader. `join()` waits for the worker to finish; it does not forcibly kill it.
5. Producer completion is marked, and the consumer is notified.
6. The consumer processes the remaining queued pulses and finishes. Main joins it.
7. Main publishes the final drained snapshot.
8. The live server stops. The MCAP logger drains its own message queue, joins its writer and explicitly closes the file.

The consumer must not exit merely because a stop request was made: the producer could still have one final batch to enqueue. Waiting for producer completion avoids abandoning that work.

Abrupt power loss or force-killing a process is different from graceful shutdown and can leave incomplete files.

## 16. Source, build, executable and deployment

**Source code** is the human-readable `.cpp` and `.hpp` text. A **compiler** translates it into machine code. A **linker** combines the compiled parts and required libraries into an **executable**, also called a binary.

CMake describes what to build and how the parts link. Conan obtains the external C++ dependencies. Docker provides the Linux build environment and toolchain. `build_script.sh` runs the build steps. Docker here does not simulate the electrical behavior of a flowmeter or provide real Pi GPIO.

The Mac executable and Pi executable are different even though both computers may have ARM CPUs: the operating systems and executable formats differ. The packaged native Mac executable supports simulation; the packaged Pi executable targets ARM64 Linux.

**Deployment** means putting the correct executable and configuration on the Pi. SSH gives you a remote terminal on it; SCP copies files between computers.

Be conscious of which terminal you are using. A laptop-side command that copies a local build file must run on the laptop, while Pi service-control commands run on the Pi, directly or through an SSH command.

## 17. Startup service and the Pi image

systemd is a Linux service manager. A `.service` file tells it which executable to run and how to manage it. Enabling the service makes it eligible to start at boot; starting it runs it now.

Useful distinctions:

- `systemctl start`: run the service now.
- `systemctl stop`: request a graceful stop now.
- `systemctl status`: inspect whether it is active or failed.
- `journalctl -u mini-drivebrain.service`: inspect the service's console logs.
- `systemctl enable`: arrange startup at boot; it does not necessarily start it immediately.

Our service expects `/home/nixos/mini-drivebrain` and accepts GPIO chip/line configuration through `/etc/mini-drivebrain.env`. A service file existing on your laptop does not mean it is installed or enabled on the Pi.

The provided Pi image uses NixOS and already defines a service. Use the team's configuration workflow or an agreed override rather than assuming a conventional Linux installation command will replace its configuration. You do not need to become a NixOS expert to understand the assignment, but you need to know which service is actually running your program.

Do not reflash a shared Pi or guess which service/configuration to replace. Coordinate that with the team when you get the hardware.

## 18. What is complete, and what remains

The software was built for Mac simulation and Linux ARM64. Tests exercised pulse processing, synchronization behavior, calibrated arithmetic, idle behavior, lifecycle cleanup, live protocol data, finalized MCAP decoding and both shutdown signals.

The actual Foxglove desktop viewer was installed, signed into by you, configured as **HyTech Flowmeter**, and verified with both live simulated data and recording playback. See `VIEWER_SETUP.md` and the saved screenshot.

No real flowmeter was connected during those checks. Actual wiring, chip/line mapping, permissions, physical pulses, Pi-image compatibility and real service startup after reboot still need the team's Pi. Simulation is valuable evidence about the shared software path, not evidence that the physical wiring works.

The GPIO kernel buffer is finite, and the application's queues are unbounded. Sustained overload, noisy input or a slow/full disk can still cause problems. This is a learning implementation, not a guarantee of zero loss under every condition.

## 19. Assignment requirements versus extras

The README requires GPIO pulse capture in its own thread; a mutex-protected deque and condition-variable consumer; Protobuf telemetry; working build integration; a main loop that safely reads and logs state; clean SIGINT/SIGTERM shutdown; a startup service; live Foxglove data; and replay of the same run from MCAP. It also requires an in-person demo and an explanation of implementation and validation.

Simulation, frequency estimation, lost-event reporting, optional calibrated volume/flow, convenience launchers, exported layouts, dependency locks and additional tests were added to help development and validation. Calibration is not an explicit requirement.

The README does not settle when or where your first Pi setup session occurs. Your assumption that you will need first-time hardware setup when you gain access is reasonable, but the team needs to allow that setup/testing time before judging a working demo.

The README also asks participants not to have an LLM write the majority of the project. Much of this implementation was AI-generated at your request. Learning it is worthwhile; be honest about that assistance and clarify with the lead how they want you to demonstrate your own understanding and work.

## 20. A concrete practice sequence on your laptop

1. Open the saved recording in Foxglove. Play and pause it. Identify `pulse_count`, `pulse_frequency_hz`, `simulated` and `missed_pulses`.
2. Explain why count rises while frequency stays near a roughly steady value. Then explain why the recorded data still plays with no live sensor connected.
3. Run `Start Flowmeter Simulator.command`, connect to `ws://localhost:5555`, and observe new live values.
4. Stop with Control-C in its Terminal. Open the newest finalized recording from `viewer-verification/recordings`. Explain why clean stopping matters.
5. Read the message definition and the queue structure. Match their fields to what you see in Foxglove.
6. Read the header, then `enqueue()`, `process_pulses()` and `snapshot()`. For each shared access, identify its mutex.
7. Read `read_pulses()` and compare real input with simulated input. Identify exactly where they converge on the same queue path.
8. Read main, then `stop()`. Describe startup and shutdown in your own words without looking at this guide.
9. If you want a small hands-on change, run a simulation at a different requested rate. Predict what happens to the count's slope and frequency before observing it. Remember scheduling can reduce the generated rate.

## 21. Code reading order

1. `proto/drivebrain_msgs.proto`: what measurements exist.
2. `drivebrain_core/include/DriverBus.hpp`: what the shared queue actually contains.
3. `drivebrain_comms/include/FlowmeterComms.hpp`: the driver interface, configuration and state.
4. `drivebrain_comms/src/FlowmeterComms.cpp`: event capture, locking, processing and lifetime.
5. `drivebrain_app/main.cpp`: how everything is connected and published.
6. `deployment/mini-drivebrain.service`: how the Pi starts the executable.
7. `CMakeLists.txt` and `build_script.sh`: how the program is built.
8. Foxglove and MCAP helpers: how schemas, channels and actual output are handled.

Do not try to memorize every dependency's implementation. You should understand the interfaces you call, the ownership and locking rules, and the path one pulse takes through your own code.

## 22. Questions to test your understanding

These are practice questions, not a promised interview question list.

- Where does the real C++ program run? Where does Foxglove run?
- What is an electrical edge, and why do we select one edge direction?
- What does the producer put in the queue? What does the consumer do with it?
- Which mutex protects the queue, and which protects the latest measurement?
- Why does `cv.wait` need a predicate and release the lock while waiting?
- Why does processing happen after releasing the queue lock?
- Why can two snapshots have the same count?
- Does a jump of two in plotted count necessarily indicate a lost pulse?
- How do pulse count, hertz and liters differ?
- What do Protobuf, WebSocket, Foxglove and MCAP each do?
- Why is localhost wrong when the server is on the Pi and the viewer is on your laptop?
- Why does main join the producer before allowing the consumer to finish?
- What did the simulator verify, and what does it leave untested?
- Does a service file on your laptop prove that the Pi starts the program at boot?

If you cannot explain an answer, return to that part of the code and trace a small example. The goal is understanding you can demonstrate, rather than a script of answers to repeat.
