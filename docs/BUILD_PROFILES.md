# Hardware and machine build profiles

Build profiles describe one complete Open Brew by Weight installation at compile time.
They separate facts about the assembled controller from facts about the coffee
machine connected to it:

| Profile | Owns | Does not own |
| --- | --- | --- |
| Hardware | ESP32 target and memory, GPIO assignments, electrical levels, relay contact and drive, installed peripherals | Coffee-machine gestures and factory settings |
| Machine | Manufacturer and model, switch topology, available feedback, factory settings | Controller pinout, relay polarity, speaker presence |
| CLI flags | Development features and deliberate per-build overrides or additions | The source-of-truth identity of either selected profile |

Profiles are version-controlled JSON files under `config/hardware/` and
`config/machines/`. They are independent and have no inheritance: each file
must completely describe its side of the installation. A role that is not
installed is written explicitly as `{ "present": false }`; it is never implied
by an omitted key or placeholder GPIO.

## Naming and available profiles

File names and `id` values use lowercase kebab-case. A hardware capability is a
suffix, so the speaker-equipped assembly is named
`esp32-s3-relay-x1-speaker.json`. This is easier to use in paths and generated
artifact names than spaces, `_w_`, or `+`, while the human-readable name remains
in `display_name`.

The initial profiles are:

| File | Meaning |
| --- | --- |
| `config/hardware/esp32-s3-relay-x1-speaker.json` | ESP32-S3 Relay X1 assembly, N16R8, passive speaker, current Open Brew by Weight pinout |
| `config/hardware/esp32-s3-relay-x1-speaker-reed.json` | Same controller assembly with the reed input physically installed on GPIO13 |
| `config/machines/rancilio-silvia-pro-x.json` | Rancilio Silvia Pro X, momentary button without machine-state feedback |
| `config/machines/rancilio-silvia-pro-x-reed.json` | Rancilio Silvia Pro X, momentary button with required reed feedback |
| `config/machines/la-marzocco-linea-micra.json` | La Marzocco Linea Micra, maintained paddle input plus cloud power observation and optional wake-gesture recognition |

The Linea Micra is this project's reference machine: the command examples
throughout the documentation select the `esp32-s3-relay-x1-speaker` hardware
with the `la-marzocco-linea-micra` machine.

Every initial integration is marked `unqualified`. The JSON is a supported
build definition, not evidence that its wiring has passed the manual electrical
and machine tests.

## Selecting profiles

List the exact built-in IDs, capabilities, and compatible hardware pairs:

```sh
./scripts/dev profiles
```

Then select exactly one hardware profile and one machine profile:

```sh
./scripts/dev build \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra
```

Each selector accepts either an exact built-in ID or an explicit JSON path.
IDs are not fuzzy-matched. Both options are mandatory for every firmware build;
there is no architecture-only build, implicit default, or remembered previous
pair. They may also come from `OPENBREWBYWEIGHT_HARDWARE` and
`OPENBREWBYWEIGHT_MACHINE`, but neither selection is saved in `.openbrewbyweight`.
USB flash and OTA require the same pair; an `--image` must carry its identity.

`--arch` is not a board selector. If supplied alongside both profiles, it can
only confirm the architecture derived from hardware; a different value fails
before compilation. Analysis tools that inspect an existing compilation
database may still use `--arch` and `--build-dir`.

The resolver performs these operations in order:

1. Parse both JSON files and reject missing, unknown, or invalid fields.
2. Apply supported `OPEN_BREW_BY_WEIGHT_*` definitions from `--flags` to a copy of the
   profile data.
3. Validate the complete resolved hardware, machine, GPIO ownership, and
   cross-profile compatibility.
4. Generate the build header and resolved manifest.
5. Compile in an isolated directory for the `<hardware>--<machine>` variant.

No flash, OTA upload, or hardware action is part of `build`.

### Capability matching

Profiles do not name or depend on one specific counterpart. The hardware
profile provides physical capabilities through each role's `present` value;
the machine profile consumes them through its selected interface. The resolver
matches the two contracts:

| Machine requirement | Hardware capability | Result |
| --- | --- | --- |
| `feedback: "reed"` | `reed.present: true` | Valid momentary+reed build (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=2`) |
| `feedback: "reed"` | `reed.present: false` | Hard error before CMake: required feedback is absent |
| `feedback: "none"` | `reed.present: false` | Valid momentary-only or paddle build |
| `feedback: "none"` | `reed.present: true` | Valid; the extra passive input is compiled as unused by that machine topology |

This keeps electrical facts in hardware and behavioral facts in the machine.
The machine profile never owns a reed GPIO, and the hardware profile never
selects momentary or paddle behavior. Each machine profile declares its
compatible hardware profiles in its `hardware` list, and the resolver rejects
any other pairing. Automated validation builds the supported built-in
pairs with `-O2`:

```text
esp32-s3-relay-x1-speaker       + rancilio-silvia-pro-x
esp32-s3-relay-x1-speaker-reed  + rancilio-silvia-pro-x-reed
esp32-s3-relay-x1-speaker       + la-marzocco-linea-micra
```

## Common profile identity

Both JSON types start with these fields:

| Field | Contract |
| --- | --- |
| `schema_version` | JSON contract version. The only supported value is `1`. |
| `id` | Stable lowercase kebab-case identifier, at most 63 characters. It becomes part of build paths and firmware identity. |
| `display_name` | Human-readable name, at most 63 characters. |
| `compatibility_revision` | Positive revision from 1 to 65535. Increment it when a change must make old and new OTA images incompatible. |

Profile objects are strict: unknown fields fail validation. Text must be
non-empty, at most 63 characters, and cannot contain control characters, `"`,
or `\`. Identity comes from `id` plus `compatibility_revision`; changing only
`display_name` does not create a new OTA compatibility identity.

## Hardware profile contract

The complete current hardware profile is:

```json
{
  "schema_version": 1,
  "id": "esp32-s3-relay-x1-speaker",
  "display_name": "ESP32-S3 Relay X1 with speaker",
  "compatibility_revision": 1,
  "metadata": {
    "product_url": "https://aliexpress.com/item/1005011880181624.html",
    "identifiers": ["303E32S3DC2", "5437314A_Y1248-250908"]
  },
  "target": { "chip": "esp32s3", "memory_profile": "n16r8" },
  "activator": {
    "gpio": 21,
    "active_level": "low",
    "pull": "up",
    "debounce_ms": 30
  },
  "relay": {
    "gpio": 2,
    "contact_type": "normally_open",
    "closed_level": "high",
    "open_level": "low"
  },
  "scale_status_led": { "present": true, "gpio": 1, "active_level": "high" },
  "speaker": { "present": true, "type": "passive_pwm", "gpio": 14 },
  "usb_console_jumper": {
    "present": true,
    "gpio": 4,
    "active_level": "low",
    "pull": "up",
    "sample_at": "boot"
  },
  "reed": { "present": false },
  "external_safety": { "present": false }
}
```

The reed-equipped variant remains a separate complete hardware profile. Its
identity ends in `-reed`, and its physical reed declaration is:

```json
"reed": {
  "present": true,
  "gpio": 13,
  "active_level": "low",
  "pull": "up",
  "debounce_ms": 30
}
```

All other roles are still written explicitly in that file; this excerpt only
highlights the capability that differs from the preceding complete example.

### Traceability metadata

`metadata` is a required but intentionally open JSON object. Its contents are
not schema-validated: maintainers may add fields such as `brand`, `author`,
`sku`, `identifiers`, `links`, fabrication notes, or nested objects without
changing the profile schema or resolver. The complete object is copied into the
resolved `build-profile.json`; it does not generate firmware macros, appear in
Diagnostic, or participate in OTA compatibility. This keeps provenance and
procurement references useful without making metadata changes incompatible with
an installed image.

The two initial X1 profiles refer to the same relay module, so both contain the
same product URL and identifiers `303E32S3DC2` and
`5437314A_Y1248-250908`. Because the resolver deliberately treats this object as
opaque data, maintainers must review it normally. Do not store access tokens,
account-specific URLs, or other secrets in profile metadata.

### Target, input, and relay

| Object / field | Values and behavior |
| --- | --- |
| `target.chip` | Currently `esp32s3`. |
| `target.memory_profile` | Currently `n16r8`: 16 MB flash and 8 MB OPI PSRAM. This describes capacity, not the physical board or pinout. |
| `activator.gpio` | Physical brew-switch input, GPIO 0–48. |
| `activator.active_level` / `pull` | Current driver requires `low` with internal pull `up`. |
| `activator.debounce_ms` | Input debounce, 1–99 ms. |
| `relay.gpio` | Relay drive output, GPIO 0–48. |
| `relay.contact_type` | Currently only `normally_open`. K1 is open at startup and on safety failure. |
| `relay.closed_level` / `open_level` | Output levels that energize/close and release/open K1. They must differ. The X1 assembly closes on `high` and opens on `low`. |

`contact_type` describes the physical contact path; `closed_level` and
`open_level` describe the controller signal. Keeping both avoids assuming that
a normally-open contact is necessarily driven by a particular logic level.

### Installed and absent resources

Every optional physical role has a required `present` boolean. If it is `true`,
all fields for that role are mandatory. If it is `false`, `present` must be the
only field; do not retain a GPIO for hardware that is not installed.

| Role | Fields when present | Current constraints |
| --- | --- | --- |
| `scale_status_led` | `gpio`, `active_level` | Level is `low` or `high`. |
| `speaker` | `type`, `gpio` | Type is currently `passive_pwm`. Presence enables the local buzzer; `-DOPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=0` omits it. |
| `usb_console_jumper` | `gpio`, `active_level`, `pull`, `sample_at` | Current driver requires active-low, pull-up, sampled at `boot`; GPIO 0, 45, and 46 are rejected as strapping pins. |
| `reed` | `gpio`, `active_level`, `pull`, `debounce_ms` | Current driver requires active-low and pull-up; debounce is 1–99 ms and must equal the activator debounce. |
| `external_safety` | `heartbeat_gpio`, `heartbeat_idle_level`, `feedback_gpio`, `feedback_closed_level`, `feedback_pull`, `heartbeat_period_ms`, `feedback_settle_ms` | Current driver requires low heartbeat idle and feedback pull-up; periods are 10–1000 ms and 1–1000 ms respectively. |

All present roles own their GPIOs exclusively. A collision between any input,
relay, LED, speaker, jumper, reed, or external-safety pin fails before CMake or
the compiler runs.

## Machine profile contract

A machine profile adds these required identity and integration fields:

| Field | Contract |
| --- | --- |
| `brand` | Manufacturer stored separately, for example `Rancilio` or `La Marzocco`. |
| `model` | Product model stored separately, for example `Silvia Pro X` or `Linea Micra`. |
| `integration_revision` | Kebab-case description of the reviewed integration. Initial profiles end in `unqualified` until physically tested. |
| `integration` | Compile-time digital integration: `none` or the allow-listed `linea_micra_cloud`. This is explicit and is never inferred from brand, model, ID, or display name. |
| `interface.control` | `momentary` or `paddle`. |
| `interface.feedback` | `none` or `reed`. Paddle currently permits only `none`. |
| `factory_defaults` | Defaults appropriate to the selected control topology. |

One optional field may also appear:

| Field | Contract |
| --- | --- |
| `machine_icon` | Kebab-case name of the machine's own icon in Home's Equipment row. The glyph catalog is the hidden `data-machine-icons` sprite in `src/web/html/home.html` (`machine-icon-default` is the reserved fallback every profile without the field keeps); the build bakes exactly the named glyph — flat shape elements in the marker site's 24×24 viewBox — into the row. |

The brand, model, and integration are not parsed from the file name, ID, or
display name. Brand and model are compiled separately and shown separately in
Diagnostic. An unknown integration fails profile resolution; adding one
requires its own isolated implementation and build registration. A
`machine_icon` that names no glyph in the Web UI catalog fails the build.

### Momentary machines

Silvia Pro X uses the historical momentary source defaults:

```json
{
  "schema_version": 1,
  "id": "rancilio-silvia-pro-x",
  "display_name": "Rancilio Silvia Pro X",
  "compatibility_revision": 1,
  "brand": "Rancilio",
  "model": "Silvia Pro X",
  "integration_revision": "momentary-only-v1-unqualified",
  "integration": "none",
  "interface": { "control": "momentary", "feedback": "none" },
  "factory_defaults": {
    "operational_wall_ms": 50000,
    "momentary": {
      "start_edge": "press",
      "stop_pulse_ms": 300,
      "max_single_press_ms": 1000,
      "assume_idle_on_scale_connect": true,
      "shot_reaction_timeout_s": 0
    },
    "quick_rinse": { "enabled": false, "gesture_ms": 1000, "duration_ms": 4000 }
  }
}
```

| Field | Accepted values | Meaning |
| --- | --- | --- |
| `operational_wall_ms` | 5000–60000 | Logical operational limit seeded for the machine. |
| `start_edge` | `press`, `release` | Edge on which firmware applies logical Start/Stop. It does not change the normal physical relay mirror. |
| `stop_pulse_ms` | 50–1000 | Pulse used to request an automatic stop. Historical default: 300 ms. |
| `max_single_press_ms` | 100–5000 | Longest hold treated as one logical press. Historical default: 1000 ms. |
| `assume_idle_on_scale_connect` | Boolean | Seeds the button-only inference behavior. Historical default: `true`. |
| `shot_reaction_timeout_s` | `0`, or 3–30 | Quiet-flow inference timeout. `0` preserves the compiled historical fallback, currently 12 seconds. |
| `reed_confirm_timeout_ms` | 200–5000 | Required only when feedback is `reed`; omitted when feedback is `none`. |

The complete reed machine profile differs in identity, selects reed feedback,
and adds the historical 1000 ms confirmation timeout:

```json
{
  "schema_version": 1,
  "id": "rancilio-silvia-pro-x-reed",
  "display_name": "Rancilio Silvia Pro X with reed",
  "compatibility_revision": 1,
  "brand": "Rancilio",
  "model": "Silvia Pro X",
  "integration_revision": "momentary-reed-v1-unqualified",
  "integration": "none",
  "interface": { "control": "momentary", "feedback": "reed" },
  "factory_defaults": {
    "operational_wall_ms": 50000,
    "momentary": {
      "start_edge": "press",
      "stop_pulse_ms": 300,
      "max_single_press_ms": 1000,
      "assume_idle_on_scale_connect": true,
      "shot_reaction_timeout_s": 0,
      "reed_confirm_timeout_ms": 1000
    },
    "quick_rinse": { "enabled": false, "gesture_ms": 1000, "duration_ms": 4000 }
  }
}
```

It only resolves with the hardware profiles the machine profile declares; the
reed machine requires hardware whose reed role is present. For example:

```sh
./scripts/dev build \
  --hardware esp32-s3-relay-x1-speaker-reed \
  --machine rancilio-silvia-pro-x-reed
```

### Paddle machines

Linea Micra uses the historical paddle source defaults, with Quick rinse enabled
for this machine profile:

```json
{
  "schema_version": 1,
  "id": "la-marzocco-linea-micra",
  "display_name": "La Marzocco Linea Micra",
  "compatibility_revision": 1,
  "brand": "La Marzocco",
  "model": "Linea Micra",
  "integration_revision": "paddle-v1-unqualified",
  "integration": "linea_micra_cloud",
  "interface": { "control": "paddle", "feedback": "none" },
  "factory_defaults": {
    "operational_wall_ms": 50000,
    "paddle": {
      "mode": "natural",
      "return_reminder": {
        "enabled": true,
        "interval_ms": 30000,
        "max_duration_ms": 900000
      }
    },
    "quick_rinse": { "enabled": true, "gesture_ms": 1000, "duration_ms": 4000 }
  }
}
```

| Field | Accepted values | Meaning |
| --- | --- | --- |
| `paddle.mode` | `natural`, `original`, `auto` | Initial paddle behavior. Historical default: `natural`. |
| `return_reminder.enabled` | Boolean | Enables the reminder while the paddle remains ON after automatic stop. |
| `return_reminder.interval_ms` | 5000–120000 | Reminder interval. Current factory default: 30000 ms. The Web selector offers OFF, 15, 30, 60, and 120 seconds. |
| `return_reminder.max_duration_ms` | At least 60000 and the interval, up to 3600000 | Maximum reminder duration. Historical default: 900000 ms. |

Both topologies require `quick_rinse`: `enabled` is Boolean, `gesture_ms` is
100–5000, and `duration_ms` is 500–10000 without exceeding
`operational_wall_ms`. The
historical timing defaults are a 1000 ms gesture and 4000 ms duration. It is
disabled in the Rancilio profiles and enabled in the Linea Micra profile.

## Defaults, persistence, and factory reset

Machine values are factory defaults, not a forced runtime policy. They seed a
new installation and are restored by factory reset. Valid settings already in
persistent storage continue to win after a normal boot, so
changing a profile default does not silently rewrite an existing user's setup.

Topology and physical wiring are compile-time choices. A persisted setting
cannot turn a paddle build into momentary, add a missing reed, move a GPIO, or
reverse relay polarity.

## CLI override and extension rules

The JSON supplies the base configuration. Definitions in `--flags` are parsed
next and supported values update the resolved manifest before its final
validation. Other compiler definitions and warnings are passed through as
additions. For example:

```sh
./scripts/dev build \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --flags "-DOPEN_BREW_BY_WEIGHT_RELAY_GPIO=3"
```

Supported physical overrides cover activator, relay, present LED/speaker/USB
jumper/reed/external-safety GPIOs, their supported active levels, and timing
values. Supported machine-default overrides cover the operational wall, rinse
settings, paddle mode/reminder settings, and momentary pulse, press, edge,
idle-assumption, reaction, and reed-confirmation settings.

The following safety rules still apply after overrides:

- Profile IDs, compatibility revisions, brand, and model cannot be overridden.
- Presence flags cannot contradict the physical profile. A GPIO cannot be
  assigned to a role whose `present` value is `false`.
- Overridden GPIOs are checked again for ownership collisions.
- `OPEN_BREW_BY_WEIGHT_MACHINE_TYPE` may only repeat the type implied by the machine
  profile; a conflicting value fails.
- A machine requesting reed feedback requires hardware with `reed.present=true`.
- Buzzer support follows `speaker.present`; forcing `=1` without a speaker fails.
- The architecture cannot contradict `target`.

Development mode, JTAG, remote machine control, buzzer enablement, warnings,
and other non-physical build switches remain CLI concerns. The `--release`
profile (the default) compiles with neither admin unlock nor the JTAG console;
the `--development` profile combines `--no-auth-admin --jtag`. `--no-auth-admin`
adds `OPEN_BREW_BY_WEIGHT_DEVELOPMENT=1` for that run without saving it and
conflicts with an explicit `OPEN_BREW_BY_WEIGHT_DEVELOPMENT=0`. These profile
and unlock switches must never be added to a JSON profile.

## Generated files and runtime identity

A named build uses:

```text
build-idf/<hardware-id>--<machine-id>/
```

Its `generated/` directory contains:

- `build-profile.json`: the fully resolved hardware and machine data after
  supported overrides;
- `OpenBrewByWeightBuildProfileGenerated.h`: compile-time macros consumed by the
  firmware, including `OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER` from `speaker.present`;
- `OpenBrewByWeightVersion.h`: firmware version plus architecture and profile
  compatibility identities.

The build also copies a named binary to
`openbrewbyweight-<hardware-id>--<machine-id>.bin` inside the same build directory.
Generated files are build outputs; edit source profiles instead of editing
them.

The firmware reports `hardwareProfile`, `machineProfile`, `machineBrand`, and
`machineModel` in status/build information. Diagnostic's **Compilation** section
shows the same resolved identity. OTA identity uses the architecture and
`<id>-r<compatibility_revision>` for both profiles; an image built for a
different hardware or machine compatibility identity is rejected even when its
ESP32 memory configuration is identical.

Architecture-only firmware builds are rejected because `n16r8` does not identify
a physical board, pinout, relay circuit, or coffee machine.

## Adding or changing a profile

1. Copy the closest profile of the same type and give it a new stable kebab-case
   file name and `id`.
2. Describe every field, including absent physical roles. Do not copy a pinout
   or machine topology based only on a product-family name.
3. Record brand and model separately for a machine. Use an `unqualified`
   integration revision until the exact circuit and machine revision pass the
   manual acceptance plan.
4. Increment `compatibility_revision` whenever the same ID changes in a way
   that must block cross-installation OTA. Use a new ID when it represents a
   different assembled controller or machine integration.
5. Run the profile contract test and an exact named build:

   ```sh
   ./scripts/dev test tooling
   ./scripts/dev build \
     --hardware <hardware-id> \
     --machine <machine-id>
   ```

6. Classify the complete change and run the required validation gate. Hardware
   qualification, flashing, relay operation, and HIL require separate explicit
   authorization and evidence.

Related references: [Build](BUILD.md), [script reference](SCRIPTS.md),
[hardware installation](HARDWARE.md), [momentary behavior](settings/momentary.md),
[paddle behavior](settings/paddle.md), and [OTA image identity](features/ota-image-identity.md).
