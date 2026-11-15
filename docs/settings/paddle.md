# Paddle

For latch/paddle firmware (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=0`), choose
**Settings → Shared settings → Paddle**. The mode is shared across presets.
Momentary builds show [Switch](momentary.md) instead.

`config/machines/la-marzocco-linea-micra.json` selects this paddle topology for
the initial La Marzocco Linea Micra integration. Factory reset seeds Natural
mode, the 30-second return reminder with a 15-minute limit, and Quick rinse on
(1-second gesture, 4-second duration). Persisted user choices remain unchanged
by normal USB firmware installation. This integration is not described as hardware-qualified until
its actual maintained-input circuit passes the manual checks.

In **Settings → Shared settings → Alerts**, choose **Paddle-off reminder**: **OFF**, **15**, **30**,
**60**, or **120** seconds. The reminder is on at 30 seconds after a factory
reset. OFF silences it. The separate **Paddle reminder limit** still stops
repeating beeps after the chosen number of minutes. A change is saved as soon
as you choose it and is shared across presets. An existing saved interval
outside the listed choices keeps running and appears in the selector as it
is, until you choose another one.

## Choose a mode

<a id="when-it-applies"></a>
<a id="parameters"></a>

| During an automatic BBW shot with a usable scale | Natural (default) | Original | Auto |
| --- | --- | --- | --- |
| Move ON | Start | Start | Start |
| Leave ON | Stop automatically by weight/guards | Weight stop and operational wall wait for OFF; electrical hard cap remains | Stop automatically by weight/guards |
| Move OFF after any enabled rinse window | Stop now | Continue automatically | Continue automatically |
| Stop early using the paddle | OFF | ON again, then OFF | ON/OFF only changes the current subtype; it is not an early-stop gesture |

Choose Natural if you want OFF to mean stop. Auto is a different workflow:
do not assume returning the paddle to OFF will stop an automatic shot.
Authenticated Web Stop and applicable limits remain available.

Without a usable scale at start, or with BBW off, OFF ends the shot in all
three modes. No-scale policy can block the start first. Max shot time does not
apply to timer-only or manual no-scale shots; the electrical 60 s cap remains.

## Natural

1. Move ON to start; start tare and the timer run when supported.
2. Leave ON for automatic stop, or move OFF to stop early.
3. After automatic stop, move OFF before starting again. The reminder can beep
   while the paddle remains ON.

With [Quick rinse](quick-rinse.md) enabled, an ON→OFF inside its gesture window
requests rinse instead. The Linea Micra profile enables it by default.

## Original

1. Move ON to start.
2. Move OFF after the rinse window, if enabled. Weight control can now finish
   the shot.
3. To stop early, move ON again, then OFF. That shot switches to Natural
   behavior; the saved mode stays Original.

With the firmware enabled, leaving ON does not grant unlimited brewing: the
hard electrical cap still applies even while weight stop and the operational
wall are held off. [Compatibility mode](firmware-mode.md) has no duration cap.

## Auto

Start with ON and leave the paddle ON or OFF after any rinse gesture window.
Weight control can stop in either position. Moving ON/OFF changes only this
shot's internal subtype; it does not switch to Natural's early-stop behavior.

## How K1 follows the paddle

K1 is the relay contact. It closes only when start is allowed. A blocked hold
(no-scale/cup-start guard or safety fault) requires release and a fresh start.
Pending idle automatic tare is not a blocked hold: an allowed move to ON starts
immediately and the shot performs its normal timer/tare sequence.

After automatic stop, K1 stays open even if the paddle is still ON; the
controller waits for stable OFF before rearming. During an allowed rinse or
Original/Auto continuation, K1 can remain closed with the paddle OFF.

The implementation's GPIO refresh and run-state rules are in
[Machine run state](../STATE_MACHINES.md#3-machine-run-state-machinerunstate).

## Example

With Natural, BBW on and Quick rinse off, move ON and stop early with OFF.
With Original, ON→OFF can instead leave the shot running. Confirm the selected
mode before interpreting a gesture as a stop.

Related: [Scale guard](no-scale-bbw.md), [BBW](../features/brew-by-weight.md),
[alerts](../alerts.md), [momentary](momentary.md).
