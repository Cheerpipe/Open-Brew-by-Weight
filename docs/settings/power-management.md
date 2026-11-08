# Power management

The **Admin → Power management** group holds the four settings that control
how much energy the controller uses, in this order. Like every other Admin
group, changes are kept in the page only until you press the group's
**Save settings** button; the arrow button beside it discards them and puts
the saved values back. Only the fields you changed are saved.

- **Performance boost** — runs the processor at full speed (160 MHz) while a
  scale is connecting or connected, and for 30 seconds after it disconnects.
  Off, the processor stays at 80 MHz during that time.
- **Idle power saving** — when the machine is not in use, lets the processor
  slow down (as low as 40 MHz) and the Bluetooth radio sleep between radio
  events. Off, the processor never drops below 80 MHz and Bluetooth stays
  awake. On a La Marzocco Linea Micra it also follows the machine's
  [power state](#linea-micra-power-state).
- **Wi-Fi sleep** — puts the Wi-Fi radio into modem sleep between the
  router's beacons while connected. Saves without restarting or waiting for a
  reconnect, and stays disabled until a network is configured.
  [Discovery by name](wifi.md#discovery-by-name) keeps working while sleep is
  on: the radio wakes for every beacon window, so name lookups stay slightly
  delayed but are not lost. Details in [Wi-Fi](wifi.md).
- **BLE scan mode** — how much radio time is spent searching for Bluetooth
  espresso scales: **Aggressive**, **Balanced** (factory default), or
  **Relaxed**. The saved mode always decides discovery duty, whatever the CPU
  power profile is. See [Scales](scales.md). The separate
  [Bluetooth on/off](scales.md#bluetooth-onoff) switch in the Admin **BLE**
  group can stop all scale Bluetooth regardless of this mode.

The four settings are independent. Performance boost and Idle power saving
manage the ESP32's processor and Bluetooth radio hardware only: Bluetooth search
intensity comes from the BLE scan mode and, on a Linea Micra, the
[machine-aware scan options](linea-micra.md#machine-aware-scale-search), and
Wi-Fi sleep is its own switch.

**Performance boost** and **Idle power saving** are global, persistent
settings. Both default **on** on a clean install and after factory reset, and
neither belongs to a shot preset. A controller updated from firmware that had a
single **ESP32 power management** switch keeps that choice for both: on
becomes both on, off becomes both off. Save them while the machine is stopped;
the existing Admin unlock and configuration revision checks apply.
When you press the group's Save button, Admin confirms the applied revision
and selected value, then waits for the
asynchronous configuration save to finish. An `APPLIED` command result alone
does not confirm durable storage. Admin status exposes `config.persistPending`
and `config.persistFailed`; a failed write is reported without undoing the live
setting, and the existing persistence worker retries it.

| Idle power saving | Performance boost | Idle | Scale connecting, connected, or within 30 s after disconnection | Shot, manual operation, rinse, recent WebUI activity, physical-use cooldown |
| --- | --- | --- | --- | --- |
| off | off | Fixed 80 MHz, Bluetooth awake | Fixed 80 MHz | Fixed 80 MHz |
| on | on | 40–80 MHz after 1 s of stable idle, Bluetooth controller modem sleep | Fixed 160 MHz, Bluetooth awake | Fixed 80 MHz |
| on | off | 40–80 MHz after 1 s of stable idle, Bluetooth controller modem sleep | Fixed 80 MHz, Bluetooth awake | Fixed 80 MHz |
| off | on | Fixed 80 MHz, Bluetooth awake | Fixed 160 MHz, Bluetooth awake | Fixed 80 MHz |

AP provisioning, STA reconnect, OTA and other maintenance, and the USB console
keep at least 80 MHz with every combination. The saved Wi-Fi sleep preference
and BLE scan mode apply unchanged in every row. A scale window that overlaps a
shot uses the scale column; the highest current demand wins.

Forty MHz is an eligible minimum: radio drivers can hold the CPU at 80 MHz.
The Admin status shows the instantaneous CPU clock and configured range, not
measured residency or electrical consumption. Board measurements are required
to quantify savings and qualify scale-discovery, HTTP and control latency.

Machine state determines how long operation remains protected. An open relay
alone does not prove a momentary machine is stopped. A scale reconnect starts
a new scale window (160 MHz with Performance boost), and the clock returns to
the normal operating range 30 seconds after the final disconnection, even if a
shot continues. The
five-minute cooldown never ends or extends a shot; existing safety limits,
including the 60-second hard cap, remain unchanged.

Opening or returning to the visible WebUI, and trusted taps, input, keys or
scroll gestures, create a three-minute activity window. Existing requests send
`X-WebUI-Activity` with at most 30 seconds of remaining presence. Automatic
polls do not extend human activity; hidden/closed/disconnected pages stop renewal
and the controller releases the WebUI requirement within 30 seconds. The last
lease is capped by the human activity deadline. No unload request is needed.
This is independent of the existing 15-minute WebUI ownership inactivity rule
and of the physical-use cooldown. The highest current CPU demand wins.

With both options **off**, CPU stays at 80 MHz, controller modem sleep is
disabled, and the saved scan intensity applies. The saved Wi-Fi sleep preference
applies independently of both options. PM support is still compiled in, so its
SDK overhead remains. No combination uses automatic
light sleep or deep sleep. The independent safety timer stays enabled on XTAL;
speaker LEDC also uses the S3 crystal clock.

CPU profiles, cooldowns and grace windows manage hardware only: they never
select or override the Bluetooth search intensity. That intensity comes from
the saved BLE scan mode and, when it applies, from the Micra
machine-aware scan options.

Admin and Diagnostic status include a `power` object with both settings
(`idleSaving`, `boost`), requested/applied profiles (`machine-on` while a Linea
Micra is known to be on), instantaneous `cpuMhz`, `minMhz`, `maxMhz`, apply
errors, BLE sleep policy, WebUI presence and remaining physical cooldown. An optional clock apply
failure latches a fixed-80-MHz fallback until reboot. If even that fallback
fails, the existing safety trip/restart path applies. A rejected BLE sleep-enable
request disables optional savings and restores awake service. Failure to disable
sleep, or a controller still asleep after 100 ms, reports a critical task fault
through the existing relay safety trip/restart path. Wake waits yield without
publishing worker progress or advancing BLE work, including when no scale is
connected. Errors remain visible in status.

## Linea Micra power state

On a La Marzocco Linea Micra with **Show power state** on, Idle power saving
follows the same machine power state Home shows, including the optimistic ON
and OFF estimates after a paddle wake or a standby command:

- **ON** — the machine is on, so the processor holds 80 MHz and Bluetooth
  stays awake while nothing else needs more.
- **OFF** — when the machine switches off, the five-minute physical-use
  cooldown ends at once and idle saving starts after the usual one second of
  stable idle. Using the paddle afterwards starts a new cooldown.
- **Unknown** (monitoring off, no account, Eco mode, or no reading yet) — the
  usual five-minute cooldown applies, as on every other machine.
- **Connection lost** — when the machine drops off the La Marzocco cloud, or
  the controller cannot reach the cloud (home internet or Wi-Fi down), the
  last known state stands in for 15 minutes: a machine that was on keeps
  80 MHz, one that was off keeps saving. Moving the paddle tells the
  controller the machine is on and starts another 15 minutes. After that the
  state counts as unknown and the five-minute cooldown applies again. The
  machine stays usable throughout: every paddle movement is a normal shot,
  except that a machine last seen off is woken by its first movement, as
  usual.

Scale activity, WebUI activity and maintenance still take priority over the
machine's state.
