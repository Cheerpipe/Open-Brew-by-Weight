# Technical glossary

Terms used across the Open Brew by Weight documentation and what they mean in
this project.

| Term | Meaning in this project |
| --- | --- |
| α (alpha) | The learning speed used by adaptive EWMA. It controls how much the latest eligible shot changes the learned drip. |
| A→M | **Automatic-to-manual** transition: brew-by-weight can no longer rely on the scale, so the scale-loss backup limits the remaining shot by time. |
| Admin unlock | A password-protected authorization required for privileged Web UI actions; it is separate from claiming the browser session. |
| AGPL-3.0 | GNU Affero General Public License version 3, the repository's current copyleft license. Network use of a modified version can require offering its corresponding source code. |
| AI | Artificial intelligence. The [disclaimer](../DISCLAIMER.md) notes its use during project development. |
| API | Application Programming Interface: a defined way for software components or external integrations to exchange commands and data. |
| ASan | **AddressSanitizer**, a host-test instrument that detects invalid memory access, such as reading past a buffer or using freed memory. |
| BLE | Bluetooth Low Energy, the wireless link used to communicate with supported scales. |
| BBW | **Brew by weight**, the feature that uses live scale measurements to request a stop near the recipe target. |
| Baseline | A saved starting value to which learned settings can be reset. It is not an extra correction added to the current value. |
| Boot / boot verification | Boot is the controller's startup process. After OTA, verification confirms that the new image started safely before it is kept. |
| Browser claim | The controller's way of assigning ordinary Home and Settings access to a browser session; it does not grant Admin privileges. |
| CI | Continuous Integration, the practice of running automated checks on every change. This repository runs its checks locally through `./scripts/dev` instead of a hosted pipeline. |
| Clamp | To restrict a calculated value to a minimum and maximum; the drip-learning formula keeps the learned drip between 0 g and 5 g. |
| Compile-time setting | A choice fixed while firmware is built. Changing it requires a new build; it cannot be changed later in the Web UI. |
| CSV | Comma-Separated Values, a plain-text tabular format used when exporting or exchanging shot data. |
| Cutoff / stop offset | The point at which the controller requests a stop before the target weight, allowing for coffee that continues to drip. The learned offset is adjusted from previous eligible shots. |
| DIY | Do it yourself: the user assembles, installs, and validates the hardware rather than receiving a certified finished product. |
| Drip compensation | Stopping early enough to account for liquid that continues reaching the cup after the machine receives the stop request. |
| ESP32-S3 | The Espressif microcontroller family on which the controller firmware runs. |
| ESP-IDF | Espressif IoT Development Framework, the supported toolchain and software framework used to build the firmware. |
| EWMA | Exponentially Weighted Moving Average, the adaptive learning method that applies a chosen fraction of the latest final-weight error instead of the entire error. |
| Firmware | Software built to run on the ESP32-S3 controller rather than on the user's computer or browser. |
| Firmware slot | One of the flash-memory regions that can hold a bootable firmware image. OTA writes the inactive slot so the previous image remains available for rollback. |
| Flash memory | Non-volatile storage that keeps firmware when power is removed; it is distinct from working memory such as PSRAM. |
| Gate | The complete set of automated checks and, where required, physical evidence demanded for a change's risk level. |
| GPIO | General-Purpose Input/Output, a configurable electrical pin used to sense a switch or control a signal. Its assignment and polarity are safety-critical here. |
| Guard | A protective rule that stops or limits a shot when measured behavior is outside the recipe's expected conditions. |
| HIL | Hardware in the loop: testing with the real controller or representative electrical hardware so physical timing and I/O behavior can be observed. |
| Host test | A test compiled and run on a computer, without operating the ESP32, relay, or espresso machine. |
| HTTP | Hypertext Transfer Protocol, used by the local Web UI, webhooks, and update-related interfaces. |
| ISR | Interrupt Service Routine, code that responds immediately to a hardware event and must obey stricter timing and concurrency rules. |
| K2 | The documentation's example name for an optional external safety relay or barrier; its actual design depends on the machine and jurisdiction. |
| MB | Megabyte, used here to describe flash and PSRAM capacity. |
| MIT License | The permissive license that applies to historical snapshots of this repository, not to its current work. |
| Momentary machine / pulse | A momentary machine changes state after a brief button-like electrical signal, called a pulse, rather than following a maintained switch position. |
| MQTT | Message Queuing Telemetry Transport, a publish/subscribe messaging protocol; persistent MQTT remote control is outside this project's goals. |
| n16r8 | The supported ESP32-S3 memory variant: 16 MB flash and 8 MB PSRAM. |
| OTA | Over-the-air update: installing firmware over Wi-Fi into an inactive firmware slot, followed by boot verification and possible rollback. |
| Paddle / latch | A maintained physical switch: its state directly represents the requested brew state. |
| PETG | Polyethylene terephthalate glycol, the minimum 3D-printing material for the controller enclosure; see [3D-printable enclosure](HARDWARE.md#3d-printable-enclosure). |
| Pinout / GPIO map | The documented assignment between board pins and electrical functions. A different board requires a reviewed assignment. |
| Preset | A saved recipe and its related brew settings, such as target weight and learned values. |
| PSRAM | Pseudostatic RAM, external working memory fitted to the supported ESP32-S3 boards. |
| Reed / Hall input | A magnetic sensor input used by the preferred momentary-machine configuration to observe whether the machine is running. |
| Regression | Here, a mathematical method that estimates drip behavior from previous eligible shots to improve the stop point. |
| Relay / isolated contacts | An electrically controlled switch whose contact side is kept separate from the controller side. The project uses it to request machine activation or stopping. |
| Rollback | Returning to the previous bootable firmware image when a new OTA image fails verification. |
| Sanitizer | A test-build instrument that watches a running program for classes of bugs that ordinary functional assertions may miss. |
| Soak test | A long or repeated run used to reveal resource leaks, timing drift, or failures that short tests may not expose. |
| Static analysis / cppcheck | Inspection of source code without executing it, used to detect suspicious constructs and rule violations. `cppcheck` is one analyzer used by the R3 gate. |
| Tare / retare | Setting the scale's current load to zero; retare repeats that action when the workflow needs a new zero reference. |
| Telemetry | Status or measurement data reported while a system runs. The Web UI is not promised as a guaranteed real-time telemetry channel. |
| TSAN | **ThreadSanitizer**, a host-test instrument that detects data races: unsafely coordinated simultaneous access to shared data by multiple threads. |
| UBSan | **UndefinedBehaviorSanitizer**, a host-test instrument that detects invalid C/C++ operations whose result the language does not define. |
| USB | Universal Serial Bus, used to install firmware, monitor serial output, and issue supported maintenance commands. |
| Watchdog | A timer that detects software that has stopped making required progress and triggers a controlled recovery. |
| Web UI | The browser-based user interface served by the controller. |
| Webhook | An HTTP request sent to another local service when a configured event occurs. |
| Wi-Fi / access point (AP) | Wi-Fi is the local wireless network technology; in access-point mode the controller creates its own temporary network for first setup or recovery. |
