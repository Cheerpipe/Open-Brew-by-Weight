# Hardware

Development board used for this firmware, default GPIO map, and wiring
warnings.

## Installation readiness

The board pin map and enclosure are documented below. Machine-specific wiring diagrams and installation photos are provided as references.

### Wiring Diagrams

- **Linea Micra**:
  ![Linea Micra wiring: paddle cable, CN9 on the relay, CN4 power and the optional buzzer](images/micra_diagram.png)
- **Rancilio Silvia Pro X**:
  ![Rancilio Silvia Pro X Wiring Diagram](images/silvia_pro_x_diagram.png)

### Installation Photos

**Rancilio Silvia Pro X**:
![Rancilio Silvia Pro X Installation 1](images/rancilio_silvia_pro_x_installation-1.png)
![Rancilio Silvia Pro X Installation 2](images/rancilio_silvia_pro_x_installation-2.png)
![Rancilio Silvia Pro X Installation 3](images/rancilio_silvia_pro_x_installation-3.png)

Do not use the photographs alone as a complete wiring diagram or assume compatibility from a machine name. Before connecting a machine, obtain and verify its circuit documentation and complete the applicable [bench tests](MANUAL_TEST_PLAN.md). This page cannot replace complete electrical verification.

On the Linea Micra, the intercepted brew-switch connector is labelled **CN9**.
This firmware treats that contact as the **machine circuit** — the isolated
path that makes the machine run — not as a Micra-specific name. Other machines
intercept a different brew/run circuit with the same relay contract. Other guides call this the **machine circuit** so the term also applies to
non-Micra installations.

## Parts checklist

| Part | Selection requirement |
| --- | --- |
| ESP32-S3 relay controller | Supported PSRAM variant; verify actual module, GPIO map and relay polarity against the board below. |
| Power source | Voltage, isolation, capacity, and protection verified for the actual board and machine. The repository does not define a universal machine power tap. |
| Wiring, connectors, mounting | Correct ratings, insulation, strain relief and clearance for the installation; machine-specific part numbers remain unverified. |
| Bluetooth scale | Check [model capabilities](../libraries/EspressoScaleBLE/README.md#scale-compatibility). |
| Reed/hall feedback | Recommended for momentary machines; input must be compatible with the ESP32 and isolated from the machine circuit. |
| Local sound | Optional passive buzzer; see [connection notes](#local-buzzer). |
| Independent safety barrier | A reviewed K2/heartbeat/feedback design where required; no universal K2 circuit is provided here. |
| USB data cable / console jumper | Used for installation and diagnostics; see [USB console jumper](#usb-console-jumper). |

The functional path is physical switch → ESP32 input → control policy →
isolated relay contacts → machine activation circuit. It is a signal-flow
description, not a wiring schematic. Paddle uses a maintained contact;
momentary uses press/stop pulses.

## Development board

Firmware was developed on an **ESP32-S3 1-channel relay** board (WROOM-1
**N16R8** module: 16 MB flash, 8 MB OPI PSRAM, USB-C, onboard Songle relay
with COM / NO / NC screw terminals). Product listing used for development:
[ESP32-S3 1-channel relay (AliExpress)](https://es.aliexpress.com/item/1005011880181624.html).

### Front

![Front of the ESP32-S3 1-channel relay development board](images/ESP32-S3_Relay_X1-front.png)

### Back

![Back of the ESP32-S3 1-channel relay development board](images/ESP32-S3_Relay_X1-back.png)

Every ESP32-S3 Relay X1 covered by this project uses the **N16R8** module. The
named `esp32-s3-relay-x1-speaker` hardware profile is the authoritative GPIO,
relay, memory, and peripheral description for the speaker-equipped assembly;
it is not a generic DevKit pinout. Classic ESP32 and Arduino Nano ESP32
are not supported.

## 3D-printable enclosure

A simple two-part enclosure is provided for the ESP32-S3 relay development
board. Choose a matching box-and-cover pair:

- **Standard:** `OpenBrewByWeight-Box.stl` with its open/partial cover
  `OpenBrewByWeight-Cover.stl`.
- **Thick:** `OpenBrewByWeight-Box-thick.stl` with its thicker cover
  `OpenBrewByWeight-Cover-thick.stl`. This pair
  better resists deformation where the ESP32 is exposed to more heat; PETG is
  recommended.

The supplied geometry is a fit reference, not a thermal or electrical safety
rating. Choose material and mounting for measured temperatures and required
clearances; a printed case does not establish suitability inside a hot machine.

Example of the assembled, printed enclosure:

![Printed Open Brew by Weight enclosure](images/case-1.png)

Named wiring lives in `config/hardware/`; compile-time safety assertions remain
in `src/OpenBrewByWeightHardware.h`. Pins are **not**
editable from the Web UI.

## Default GPIOs

| Function | GPIO | Level |
| --- | ---: | --- |
| Activator (to GND) | **21** | Active **LOW** (internal pull-up; ON = GPIO LOW). Paddle or switch, depending on machine type. |
| Reed (momentary+reed builds) | **13** | Active **LOW** (internal pull-up; ON = GPIO LOW). Compile `OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=2`. Override with `-DOPEN_BREW_BY_WEIGHT_REED_GPIO`. Must stay distinct from activator, relay, LED, buzzer, USB console, and safety GPIOs. |
| Onboard relay coil | **2** | Active **HIGH** (HIGH energizes the coil and closes NO) |
| Scale-connected LED | **1** | Active HIGH while a BLE scale is connected; fast blink while connecting, slow blink on a stale weight stream (switchable in Alerts) |
| Optional buzzer | **14** | Present on speaker builds (passive piezo, RTTTL); sounds are compiled in from the hardware profile. `-DOPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=0` omits them. |
| USB console jumper | **4** | Active **LOW**. Dupont **IO4 → a GND pad you choose**. Sampled once at boot. **Do not** jumper IO4 to **EN** (that column is reset). Override with `-DOPEN_BREW_BY_WEIGHT_USB_CONSOLE_GPIO`. Must stay distinct from activator, relay, LED, buzzer, reed, and safety GPIOs. |

Optional external K2 safety (both pins or neither; no defaults, because they
depend on a reviewed board):

- `OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO`
- `OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_GPIO`
- `OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_CLOSED_LEVEL` (optional; default LOW)

Compile example (GPIO overrides). Add
`-DOPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1` only for an explicit
opt-in remote-control development build; default firmware leaves start/rinse
on the physical activator:

```sh
./scripts/dev build --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --flags "-DOPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO=16 -DOPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_GPIO=17"
```

To use a different paddle, relay, LED, buzzer, or USB-console pin, add or edit
a reviewed hardware profile (or use the matching explicit `-D` override)
and rebuild. A profile must explicitly list every supported optional role;
absent hardware uses only `{ "present": false }` and has no fictitious GPIO.
Wrong pins can leave machine circuit closed or misread the paddle.

## USB console jumper

App USB CDC is **off** unless GPIO 4 is held LOW at reset (Dupont to GND),
or the firmware was compiled with `-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1`. Leave the
pin floating when the controller is inside the machine. Pull the Dupont
**before** installing.

- **With jumper at reset (default firmware):** `/dev/cu.usbmodem*` /
  `/dev/ttyACM*` appears; [USB serial CLI](SERIAL_CLI.md) and `monitor-idf`
  work.
- **JTAG build (`-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1`):** USB Serial/JTAG is on at
  boot (OpenOCD + CDC) with no jumper. Do not ship this to a machine build.
- **Without jumper, default app running:** no CDC port. Flash with
  **BOOT + RST** (ROM USB download).
- ROM download always works, jumper or not. On this board IO4 sits next to
  **EN** — never short that pair.

## Isolation (must)

- The machine circuit connects only to the relay **COM/NO** contact. Never to GND, VCC, or an
  ESP32 GPIO.
- Do not use NC as the brew path.
- Feedback, if you add it, must also be isolated. Do not join machine circuit ground to
  ESP32 ground.
- The onboard relay is **not** a certified safety barrier. A welded contact
  or a shorted driver needs a second, normally-open contact (K2) driven by an
  independent heartbeat. Without that, protection is software-only.

Verify continuity, module polarity, and that the relay stays **open** during
startup, reset, and power loss.

## Scale-connected LED

GPIO 1 is HIGH while a BLE scale is connected and **Settings → Shared settings → Alerts → Blue
LED while scale connected** is on. It blinks fast while a scale connects and
slowly when a connected scale stops sending weight. It is diagnostic only and
is never part of the machine-circuit decision.

Override at compile time with `-DOPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_GPIO=…`.
The pin must be output-capable and distinct from paddle, relay, buzzer,
heartbeat, and feedback.

## Local buzzer

Wire a **passive** 3.3 V piezo/speaker to `OPEN_BREW_BY_WEIGHT_BUZZER_GPIO` (default
14). The firmware drives it with PWM (RTTTL melodies), so it must be
**passive, not active**. A module with three pins — **GND**, **VCC** and
**IN** — is preferred over a loose two-wire speaker: connect GND to GND, VCC
to 3.3 V, and IN to the buzzer GPIO. For a two-wire passive piezo, connect
the marked **+** to the GPIO and the other lead to GND.

Example used: [3.3 V passive buzzer module (AliExpress)](https://es.aliexpress.com/item/1005007287329656.html).

`-DOPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=0` omits the driver. See [Alerts](alerts.md) and
[Build environment](BUILD.md).

## Additional hardware used

### Reed switch (optional, strongly recommended for momentary machines)

The reed switch is optional, but is strongly recommended for momentary-button
machines such as the **Rancilio Silvia Pro X**. Without it, firmware combines
its internal state machine with scale readings to infer whether the machine is
running. With a reed switch on the solenoid, it reads the machine state
directly, which is substantially more reliable and precise.

Use it with the momentary+reed build (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=2`); the
default input is GPIO 13, as documented in [Default GPIOs](#default-gpios).
It is generally unnecessary for latch/paddle machines such as the **Linea
Micra**.

Example used: [reed switch sensor module (AliExpress)](https://es.aliexpress.com/item/1005002797173105.html).

Related: [Disclaimer](../README.md#disclaimer), [FAQ](FAQ.md).
