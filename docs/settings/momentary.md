# Momentary

For firmware built with `OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=1` (button only) or `2`
(button plus reed/hall sensor), use **Settings → Shared settings → Switch**.
Paddle firmware hides this group.

`config/machines/rancilio-silvia-pro-x.json` and
`config/machines/rancilio-silvia-pro-x-reed.json` provide momentary profiles.
The base profile infers state without a sensor; the `-reed` variant requires
the matching reed-equipped hardware profile. Both retain the historical source
defaults: button press, 300 ms stop pulse, 1,000 ms single-press limit, idle
assumption on scale connect, the compiled 12 s reaction fallback, and Quick
rinse off. Their integration revisions remain unqualified until each exact
machine revision and circuit passes the manual electrical checks.

## Start and stop

Press and release the brew button to start; a second valid press requests stop.
The relay normally mirrors the physical hold 1:1. **Start/stop on** chooses
whether the firmware starts its timer and tare on the debounced press or release;
it does not change the normal electrical mirror.

A hold exceeding **Longest tap** is mirror-only when Quick rinse is off.
In press mode, the tentative logical start/stop is undone; in release mode,
it is never applied. With [Quick rinse](quick-rinse.md) enabled, a long hold
that starts from idle requests a timed rinse instead. If the [scale guard](no-scale-bbw.md)
is Armed, that rinse does not run water unless **Rinse without a scale** is
on; the hold still uses up the missing-scale warning.

A [no-scale](no-scale-bbw.md) or [cup-start](../features/cup-protection.md)
refusal keeps the relay open for the entire attempt. Release the button,
resolve the condition, then press again.

Pending idle automatic tare is not a refusal. An allowed gesture starts on the
configured press or release edge without a retry, and the shot performs its
normal timer/tare sequence. The physical hold is still mirrored only once; the
firmware does not add another start pulse. An enabled long-hold Quick rinse has
the same priority over pending idle tare.

## Stopping and time limits

A momentary machine keeps its own running state. Opening the controller relay
ends a button press; it does **not** necessarily stop water flow.

- **Button only:** the controller infers running state from fresh scale flow.
  Automatic cut requires confirmed running state. With no scale, a tap can
  start the machine without enough evidence for an automatic stop pulse.
- **Button + reed:** sensor state is authoritative outside the brief
  confirmation window. Automatic cut also requires that a stable sensor OFF
  has been seen earlier this boot; a stuck-ON input is not blindly pulsed.
- **BBW off / no scale:** no weight cut and no Max shot time. The 60 s logical
  cap requests a stop only when the machine-state conditions allow it.
- **Electrical cap:** holding the relay closed for 60 s opens K1 regardless.
  This limits the contact hold, not all possible machine brew durations.

Use reed feedback for a more reliable installation. Check the actual group,
not only the UI label, when inference is uncertain.

## If the displayed state is wrong

On button-only firmware, Home **Mark as idle** / **Mark as running** correct
the inferred state without pulsing the relay. They require the browser claim
but not Admin unlock. First observe the machine, then choose the matching
state. **Mark as running** is refused while the controller is still stopping a
shot it ended (until that stop is confirmed or retried), so a mis-tap cannot
leave the machine pouring. They are hidden on paddle and reed builds.

Remote Start/Stop synthesize configured pulses only under the applicable
permission policy. **Force press**, on an explicitly enabled remote-control
build after Admin unlock, sends one raw pulse without changing the logical
cycle. It is an advanced recovery action; a pulse can toggle in either
direction and still obeys electrical safety. Default firmware disables
close-producing remote controls.

## Parameters

| Setting | Default | Values | Effect |
| --- | --- | --- | --- |
| **Auto-stop pulse (ms)** | 300 | 50–1000 | Length of the pulse the firmware sends to mimic a single button press when it needs to stop the brew automatically (target weight, time walls). |
| **Longest tap (ms)** | 1000 | 100–5000 | Longest hold that still counts as a single press (start or stop). Applies in **Pressed** and **Released**. A longer hold is not a start/stop (for example a machine rinse): the relay still mirrors it. In press mode the tentative start/stop is undone and, on reed builds, a confirm-timeout grace runs before reed is canonical again. In release mode the edge is never applied. |
| **Start/stop on** | Pressed | Pressed / Released | When firmware treats the shot as started or stopped, and when the reed confirm window starts. Does not change the 1:1 relay mirror except when a start-guard blocks forwarding. |
| **Assume idle when the scale connects** | ON | ON / OFF | Switch-only (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=1`). When the scale connects, treat the group as idle. Does not pulse the relay. Skipped while a brew cycle is active. |
| **Shot reaction timeout (s)** | 12 | 3–30; `0` in JSON is the compiled 12 s | Switch-only. How long a quiet pan after Start may show **Starting** before it changes to **Stopping**. Does not pulse the relay. Late espresso-like flow while **Stopping** still switches to **Running**. If **Starting** or **Stopping** lasts until the firmware hard cap (60 s, `HARD_MAX_CIRCUIT_CLOSED_MS`) with a live scale and net mass still within 1 g of the shot baseline (noise, not espresso-like flow), the group returns to **Idle** without a pulse or beep so the next press is a new Start. |
| **Brew sensor wait (s)** | 1.0 | 0.2–5 | Momentary+reed only (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=2`). How long after the Start/stop on edge the group may show **Starting** while the reed is still off (or **Stopping** while the reed is still on). The clock starts on that press or release, not when the relay mirrors the hold. If the reed matches sooner, confirm immediately. When the timeout elapses, confirm the actual reed. |

## Examples

- **Normal press, reed attached:** press and release; the sensor confirms the
  group started. At the weight endpoint, a stop pulse is sent and the sensor
  reports the result.
- **Button held while a start guard blocks:** the hold is not forwarded even
  if a scale connects during it. Release, then press again.
- **Button-only, no scale:** the machine may start from a tap, but the brew orchestrator
  cannot promise a timed automatic stop. Remain responsible for the physical
  machine's stop action.

The detailed assumed/confirmed states, quiet-flow handling, and retry behavior
are in [Machine run state](../STATE_MACHINES.md#3-machine-run-state-machinerunstate).
Related: [Hardware](../HARDWARE.md), [Paddle](paddle.md).
