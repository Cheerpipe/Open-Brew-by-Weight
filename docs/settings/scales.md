# Scales

Finding a compatible, connectable scale pauses outbound cloud calls, webhooks
and time synchronization before Bluetooth setup begins. They resume after the
scale is ready and its quiet interval has ended. If it disappears, the controller
waits for five seconds of actual scanning without another candidate. This also
applies to machine profiles without Micra integration. Wi-Fi stays connected
and local settings remain accessible.

Which Bluetooth scale to use, how long to wait after a shot for drip, and
Bookoo-specific volume / combined tare. Machine-level, under
**Settings → Shared settings → Scale**.

Choosing a **My scale** saves it right away, like every other setting on the
page. While a saved scale is the selected value, the dropdown carries a
**Forget** key (×) inside it, left of the arrow: the first tap arms it and a
second tap forgets the scale — it leaves the saved list with its rename, and
searching pauses for 30 s so you can turn the old scale off or turn the new
one on. A scale that is still on can be found again, unnamed. **(rename)**
is a separate action that takes effect when confirmed.

Daily defaults assume a **Bookoo** Themis Mini or Ultra. **Acaia**,
**Felicita**, **AtomHeart Eclair**, **Decent**, **DiFluid Microbalance**,
**MyScale**, **Varia AKU**, **Eureka Precisa** (named GAP), and **WeighMyBru**
are also supported through the vendored EspressoScaleBLE library. Compatibility
details:
[Scale compatibility](../../libraries/EspressoScaleBLE/README.md#scale-compatibility).

Timemore Black Mirror DUO, Timemore Dot, Acaia Umbra, and Eureka units that
advertise no GAP name are not supported.

This controller connects to scales without authenticated Bluetooth pairing. Use
weight-controlled brewing where nearby Bluetooth devices are trusted: a compatible
transmitter could send misleading weight readings. The machine still requires
a physical start, and implausible or stale readings suspend weight control.

## Rename a scale

Scales broadcast names such as `BOOKOO_SC 715097`; you may still want to
distinguish two scales of the same model. Next
to the preferred scale on **Home**, on **Diagnostic → Connected scale**, and beside
**My scale** in **Settings → Scale** — where the link follows the
scale chosen in the dropdown, so you can rename any remembered scale, not
just the one that is connected — choose **(rename)** to give that scale your
own name, for example `Espresso corner scale`. Your name replaces the
broadcast name everywhere the scale is listed — Home, Diagnostic, and the
preferred-scale list in Settings — and stays with that scale across restarts.
Any remembered scale can have its own name, whether or not it is preferred.
Diagnostic shows the name of the scale connected now, or **Not connected** when
there is no connection. Home continues to show the saved preferred scale.

Bookoo Themis models identify themselves in that broadcast name, so the
scale library proposes a readable model name for you: a scale broadcasting
`BOOKOO_SC` with digits appears as **BOOKOO Themis Mini**, and one
broadcasting `BOOKOO_SC_U` with digits appears as **BOOKOO Themis Ultra**.
The proposed name appears when the scale is first detected and added to the
list, and keeps working after a restart. A name you choose always takes
precedence.

Use up to 30 letters, numbers, spaces, or hyphens. Saving with an empty text
field returns to the proposed model name, or to the broadcast name when the
model is unknown. Renaming is unavailable while a shot is
running.

## Reading scale diagnostics

**Supported commands** lists the commands available for the connected,
identified Bookoo model. Themis Ultra lists tare, volume, start/stop/reset
timer, tare and start timer, and power off. Themis Mini lists the same commands
except power off. Ultra may ignore power off while charging. An unidentified
model shows that command support is unknown.

When no scale is connected, the Bluetooth symbol and its signal bars in the
header share the same faded appearance in both light and dark modes.

In **Diagnostic → Scale**, **Weight updates** shows the recent, smoothed
interval between weight readings and the corresponding rate. **Scale gaps**
counts pauses longer than 250 ms between consecutive readings while the scale
is connected and ready for weight control. The count lasts until the controller
restarts; it does not count individual missing packets or time disconnected.

**Longest gap** shows the longest of those pauses in milliseconds since
startup or its last reset. Select **(Reset)** beside it to begin a new maximum
measurement; this leaves the Scale gaps count unchanged. A value of **0 ms**
means no qualifying pause has been recorded in that measurement period.
The maximum remains visible after readings recover or the scale reconnects.
It is recorded when the next reading arrives, so it does not measure an ongoing
silence. Use it alongside Stream, Control and Reconnects when investigating
delayed readings; the duration alone does not establish the effect on a shot.

## Bluetooth on/off

**Admin → BLE → Enable Bluetooth** is the master switch for Bluetooth scales.
It is the first group on the Admin page and defaults to **on**. The switch
only takes effect when you press the group's **Save settings** button; the
arrow button beside it discards the change and restores the saved position.

- **On** (default): scales are searched for and connected exactly as always.
- **Off**: searching stops and any connected scale is disconnected within a
  moment. No new scale can connect until the switch is turned back on.

With the switch off, the controller behaves exactly as if no scale existed:
brew by weight falls back to the behavior described in
[No-scale BBW](no-scale-bbw.md), and reconnecting after enabling again takes
a few seconds because the controller honors the standard quiet period that
follows every scale disconnect. The setting changes nothing else — scan mode,
preferred scale, history, and every other setting keep their own values, and
turning Bluetooth back on restores normal operation without a restart.

## Scale power-off support

**Turn the scale off with the Micra**
([La Marzocco Linea Micra](linea-micra.md#turn-scale-off-when-the-machine-powers-off))
switches the connected scale off over Bluetooth, but only when the scale
itself accepts a power-off command. The explicit support list:

| Scale | Power-off over Bluetooth |
| --- | --- |
| Bookoo Themis Ultra | Yes — BooKoo's published protocol, command `0x15`; may be ignored while charging |
| Bookoo Themis Mini | No — BooKoo's published protocol for the Mini has no shutdown command |
| Acaia (Lunar, Pearl S, Pyxis, Cinco, Proch) | No — no power-off command is documented for the Acaia protocol |
| Felicita (Arc) | No |
| AtomHeart Eclair | No |
| Decent Scale | No |
| DiFluid Microbalance / Ti | No |
| MyScale KP2048B | No |
| Varia AKU / Mini / Pro | No |
| Eureka Precisa | No |
| WeighMyBru | No |

When the broadcast name identifies a Themis Mini, the controller skips the
shutdown command and records a warning. It sends shutdown to a recognized
Ultra. If a Bookoo broadcast does not reveal the model, the controller keeps
the existing generic Bookoo behavior, so shutdown may be attempted. Other
scales without power-off support receive no shutdown command. Scale support
assumes the current manufacturer firmware.

For Bookoo, the controller leaves at least 100 ms between Bluetooth commands.
Once it asks the scale to shut down, it sends no more commands on that
connection. The BLE library blocks commands, reads, scanning, and reconnection
for 3,000 ms, starting before the shutdown command and restarting the full
interval as soon as the scale's disconnect is observed. The first valid
advertisement after that quiet period can reconnect normally, once the previous
connection has fully closed. If a command times out during shutdown, the
controller waits for the old connection to close before searching again; a
delayed disconnect starts a fresh three-second pause.

## Manual tare for diagnosis

With a Bookoo connected and the controller **Ready**, use **Diagnostic → Scale
→ Tare** to request one standalone tare. It zeroes the scale without starting
or resetting its timer, changing volume, or sending another scale command.
The button is unavailable during a cycle. For an isolated test, turn off
automatic tare outside a brew and late-cup retare beforehand; this button does
not change those settings. **Tare requested** confirms the request was queued;
check the scale's reading and diagnostic log for its result.

**Diagnostic → Scale → Supported commands** lists the commands implemented for
the connected, identified Bookoo model in a two-column table, with command names
on the left and their hexadecimal codes on the right.
When the model is unknown or another brand is connected, the page says that
command support is unknown. The Ultra's power-off command can be ignored while
the scale is charging.

## When it applies

A usable scale is required for automatic brew-by-weight. If the scale is
missing, see [No-scale BBW](no-scale-bbw.md). If it drops mid-shot, see
[Scale-loss backup](../features/auto-to-manual.md). A Bluetooth link that
goes silent, because the scale moved out of range or lost power without
saying goodbye, is given up after two seconds, so the controller starts
searching for the scale again promptly while a brief radio hiccup still
leaves the link, and your tare, untouched.

**Drip delay** runs after machine circuit opens. It is used for Last Shot, history,
drip learning, and the scale-loss backup's recent shots.

## Parameters

| Setting | Default | Range / notes | Effect |
| --- | --- | --- | --- |
| **Which scale** | My scale first | Any scale / My scale first / Only my scale | **Any scale** connects whichever compatible scale appears first and never locks it. **My scale first** waits briefly for the preferred scale, then accepts another. **Only my scale** connects only to the saved preferred scale. If either selected mode has no saved scale yet, it uses the bootstrap flow below. |
| **My scale** | First detected | First detected, No preferred, or a BLE-seen scale | **First detected** is shown only while no preferred MAC is saved and **My scale first** or **Only my scale** is active. The controller scans by compatible name and adopts the first scale that completes a successful connection; advertisements and failed connections are not enough. **Any scale** instead shows **None saved** because that mode never locks a scale. The dropdown's **Forget** key removes the scale from the saved list, rename included, and pauses discovery for 30 s; the Scale preference mode stays. |
| **Drip delay (s)** | 3.0 s | 0–10 s | Wait after a shot ends before capturing the final post-drip weight. `0` finalizes on the next control loop with no intentional window. |
| **Scale timer stop delay (ms)** | 0 ms | 0–1000 ms | Pad after the scale timer catches up to circuit whole seconds, before `STOP_TIMER`. `0` stops in that same instant. Does not delay the local machine circuit beep. |
| **Bookoo combined command** | ON | ON / OFF | Combined tare + start-timer. Requires automatic tare at shot start. Also listed under [Tare](tare.md). |
| **Mute the Bookoo speaker** | ON | ON / OFF | Bookoo/generic: send silence (volume 0) after the first valid weight on the first connection of that scale in this Open Brew by Weight session. Reconnecting does not resend it. Applies only in **Buzzer only**. |
| **Scale volume** | 3 | 1–5 or Disabled | Bookoo/generic: set after the first valid weight on the first connection of that scale in this Open Brew by Weight session. Mini accepts levels 1–5; Ultra accepts 1–3. **Off** sends volume 0. A previously saved 4 or 5 remains saved but is skipped with a warning on a recognized Ultra. Reconnecting does not resend it. Changes that alter the effective speaker volume also apply to a stable link; saving unrelated settings does not resend the command. Applies only in **Scale only** and **Scale first**. |
| **AtomHeart Eclair** | informational | — | Uses normal tare/timer commands. No configurable volume, beep, mode, combined command, or documented command sound. In Buzzer only and Scale priority, alerts use the local buzzer; Scale only omits unsupported sounds. |

If the scale disconnects or **notifications go silent** during an automatic
extraction, weight control is suspended. Rejected brew samples (post-tare,
slew) and a stable accepted weight do not count as a lost scale. Recovery
needs three coherent samples on the current connection. Physical stop behavior depends on the selected switch/mode; applicable time
limits remain in force.

## Example

The integrated scale protocols expose weight/timer readings and firmware
command results, but no verified physical-button tare notification. Preserving
a cup at zero after such a button press is therefore not supported by the
current integration; weight alone cannot distinguish it from cup removal.
Firmware idle tare preserves the known cup reference. See [Tare](tare.md).

On a new controller, **My scale first** and **First detected** are selected.
Turn on your Bookoo: after its first successful connection, its MAC and name
replace **First detected** and are saved. From then on, the controller waits
briefly for that scale before accepting another compatible one. After each shot, the firmware
waits 3 s of drip before storing the final weight used for offset learning.
Bookoo volume is not rewritten on reconnections during the same controller
session. Restarting the controller or explicitly clearing that preferred
scale opens a new first-connection session for it.

If no compatible scale is available, the controller stays in the bootstrap
name scan indefinitely and does not silently change the saved preference.
Choosing **My scale first** before a scale has been adopted uses the same
bootstrap; after adoption, its normal preferred-first fallback applies.
When **My scale first** is active and a scale is already saved, Home suggests
switching to **Only my scale**; its button changes this setting for you.
Saving a changed preference mode or selected scale restarts an in-progress
search with the new filter. If **Only my scale** is enabled while a
different scale is connected, that connection is closed before directed
discovery begins.

If you cancel a connection just as it completes, the controller closes that
connection before starting another search. It waits for Bluetooth to confirm
the closure; if searching remains stuck because that confirmation never
arrives, restart the controller.

If a visible scale repeatedly refuses a Bluetooth connection, the first few
attempts remain quick and later attempts spread out, up to five seconds apart.
The controller keeps trying automatically; fresh weight after a healthy
connection restores the quick initial timing. A scale that stops advertising
does not trigger blind connection attempts.

With INFO serial logging enabled, `ble tx` shows each submitted Bluetooth
command once, with its name and hexadecimal bytes. For commands, `command done`
reports the result and elapsed time; `gap_ms` shows how long it has been since
the previous command. A result with `submitted=0` means the command was not
sent. The scale address is not included.

## Replace a scale or diagnose a missing connection

Home distinguishes **No sample** (connected but no accepted weight yet), **Stale**
(last weight too old), and **Disconnected**. A saved scale name or last displayed
weight does not prove the connection is usable. The first valid packet has a
5 s deadline; Bookoo disconnects after 8 s without valid packets. Automation
rejects readings older than 1 s, before that link timeout. Check
**Cup → Cup tare** for the separate placement/readiness requirements.

1. Turn off other compatible scales and close phone apps connected to the one
   you want.
2. Select the intended remembered scale (it is saved at once), or use the
   dropdown's **Forget** key for a new one. Forgetting removes the scale
   from the saved list, rename included, and pauses discovery for 30 s —
   the pause is the window to turn the old scale off, because a scale that
   is still on can be found and saved again.
3. With **Only my scale** and no saved preference, let the intended scale
   complete its first connection. Check Home for fresh weight, not just a name.
4. If discovery is slow, check **Admin → Power management → BLE scan mode**:
   factory **Balanced**, with **Aggressive** searching harder and **Relaxed**
   trading some speed for less radio time. The saved mode always decides how
   hard the search runs; [ESP32 power management](power-management.md) only
   manages CPU and radio hardware and never changes the search intensity.
   On a La Marzocco Linea Micra, the
   [machine-aware scan options](linea-micra.md#machine-aware-scale-search)
   can search harder while the machine is on and ease off while it is off.

Related: [Brew by weight](../features/brew-by-weight.md), [Tare](tare.md),
[Alerts](../alerts.md).

## Recording what the scale reports

When a scale misbehaves in a way settings cannot explain, the diagnostic page
can record a trace of everything the scale sends and every tare, cup,
touch, and first-drop decision that follows. See
[the diagnostic guide](../FAQ.md)
for how to start, stop, and download the recording. Capture lasts until its
recording space fills or you stop it; the page shows capacity used, elapsed
time, and a smoothed estimate of time remaining based on the arriving data.

Use **Diagnostic → Scale recording** to investigate unexpected cup detection,
tare, or weight-controlled brewing. Start a capture, reproduce the behavior,
stop the capture, and download the text file. For example, start with an empty
pan, place a cup, wait for automatic tare, then lift the cup again.

The recording includes received weights and the controller's interpretation:
cup placement/removal, waiting for the pan or cup to settle, readiness for a
cup, tare requests and results, uncertain references, and related brewing
states. A successful tare command and a confirmed zero are separate events.
Waiting or blocked states include reasons where the controller knows them.

You can start while an operation is already underway. The first observation
establishes its current state; later entries record changes without repeating
unchanged states. The recording cannot recover events that happened before
you started it. Cup events describe what the controller detected from the
weight, rather than an independent measurement of the physical cup.

A capture can also stop when its buffer fills. A busy session fills it sooner
because weights and state changes share that space. Check the stop reason and
lost-record count before treating a trace as complete. Completed captures are
saved when safe and survive a restart; the download indicates whether saving
has completed. Starting another capture replaces the previous one.
