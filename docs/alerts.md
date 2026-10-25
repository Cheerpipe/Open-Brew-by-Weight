# Alerts

Sounds and the optional onboard LED that tell you what the controller is
doing: tare, first drops, paddle left ON, scale lost or connected, and
extended-shot pulses.

First-drop sound plays at most once per shot. A confirmed late-cup retare can
clear an earlier first-drop record, but cannot recall a sound already played.
A replacement first drop updates the shot data without another first-drop cue.

This page covers both the feature and the **Settings → Shared settings → Alerts** group. All of
these settings are **machine-level** (not per-preset).

## When it applies

**Sound alerts** is the master switch (default ON). Off silences operational
alerts on both the local buzzer and the scale; the individual switches keep
their positions.

Most sounds follow their event: tare/start/stop, first drop, paddle reminder,
completion extra, and scale-loss / no-scale / scale-link alerts follow **Play alerts on** when a
local buzzer is compiled in (hardware builds with a speaker; passive piezo
with RTTTL). `-DOPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=0` omits the local buzzer. Channel selection is independent
of that flag, **except shot start and
stop**: those two cues always play on the local buzzer at the machine circuit relay
edge (close = start, open = stop), including auto, manual, and rinse.
They never wait for Bluetooth or for the scale timer to start or stop.

During a shot, the Home shot card shows a **touch hold** capsule while the
platform reads a sustained unexpected load, such as a finger resting on it.
It is visual only: it makes no sound and stops nothing by itself. Weight
stops wait for the load to make sense again; see
[BBW](features/brew-by-weight.md#when-touch-protection-delays-a-stop).

A **Touch fallback** ending uses the same completion cue as other automatic
stops, subject to **Sound alerts** and the available sound output.

A recognized Linea Micra standby wake is relay-only and intentionally silent:
it does not play shot start/stop, missing-scale, or missing-cup cues. See
[Linea Micra](settings/linea-micra.md#recognize-paddle-wake-gestures).

If the local tare cue is sounding or waiting when the machine circuit starts,
the shot-start cue cuts it off and plays immediately. A sound that the scale
itself has already started as command feedback cannot be recalled over
Bluetooth, so that scale sound may finish while the local shot-start cue begins.

Without buzzer support, Play alerts on and the local-only switches are hidden.
The default channel is then **Scale only**.

Start/stop cues are local feedback, not independent proof that water flow
changed. On momentary machines, check the actual group and its state feedback.

## Parameters

| Setting | Default | Range / notes | Effect |
| --- | --- | --- | --- |
| **Sound alerts** | ON | ON / OFF | Master switch in **Settings → Shared settings → Alerts**. |
| **Play alerts on** | **Buzzer only** with a local buzzer; **Scale only** without | Buzzer only / Scale only / Scale first | Where most alerts play. **Buzzer only**: all sound on the local buzzer. **Scale only**: scale path; scale-incapable local cues are muted. **Scale first**: scale when connected/able, else buzzer; never both for one event. Shot **start** and **stop** always use the local buzzer at machine circuit when a buzzer is compiled in. Shown only with buzzer support. |
| **First drops** | ON | ON / OFF | One beep on first coffee drops during an automatic shot. Ignored when brew by weight is off. |
| **Paddle-off reminder** | 30 s | OFF / 15 / 30 / 60 / 120 s | Repeat beeps at the chosen interval while the **physical paddle stays ON** and **machine circuit is already open**; OFF silences them. |
| **Paddle reminder limit (min)** | 15 min | 1–60 min | Stop beeping after this time even if the paddle remains ON. |
| **Scale lost** | (buzzer builds) | ON / OFF | Descending RTTTL on the local buzzer when the scale disconnects (idle or during a shot). Hidden / disabled when Play alerts on is Scale only. |
| **Shot finished by backup time** | (buzzer builds) | ON / OFF | Distinct RTTTL on the local buzzer when the [scale-loss backup](features/auto-to-manual.md) finishes a shot by time after the scale dropped. Disabled when Play alerts on is Scale only. |
| **Manual without scale (BBW on)** | (buzzer builds) | ON / OFF | Warns on the local buzzer that Brew by weight cannot start normally because the scale is missing, including the first blocked attempt. See [No-scale BBW](settings/no-scale-bbw.md). |
| **Scale connected** | ON (Buzzer only or Scale first) | ON / OFF | Rising RTTTL when a scale connects or reconnects. Always the local buzzer. |
| **Blue light while connected** | ON | ON / OFF | Onboard light on GPIO 1: steady while a scale is connected, fast blink while it connects, slow blink when a connected scale stops sending weight. Independent of Sound alerts. |
| **Fast-shot beeps** | Fast | Disabled / Slow / Medium / Fast / Rapid | Local-buzzer pulses while a fast shot continues past its target. Disabled when Play alerts on is Scale only. |
| **Slow-shot beeps** | Fast | Same rates | Pulses while a shot runs past your shot time window. |
| **Bullseye melody** | OFF | ON / OFF; Buzzer only | Plays the saved custom RTTTL tune once when a shot stopped by weight **On recipe** remains exactly at its target weight for at least 1 full second. Important alerts, such as a lost scale, interrupt the melody so you never miss them. The text is retained when disabled. |
| **Custom RTTTL** | empty | Up to 500 ASCII characters / 250 notes | Editable once Bullseye melody is on; otherwise it stays visible and read-only. Bullseye melody and its tune are saved together: switching it on saves nothing until you paste a tune, and a valid tune then saves both by itself. A tune that isn't valid RTTTL is marked and neither is saved. |

## Bullseye timing

Bullseye applies only to automatic shots stopped by weight whose verdict is
**On recipe** (they reached the target inside the shot time window); fast,
slow, manual, timer-only and limit endings never play it, nor do rinses.
It is active only when **Sound alerts** is on, **Play alerts on** is **Buzzer
only**, and a local buzzer is compiled in.

Tracking starts when the configured drip delay finishes. Repeated fresh scale
samples must report the target exactly for a continuous 1 full second; a
different weight or a gap longer than the normal 1-second automation freshness
limit restarts the stability timer. There is no further time limit while the
same cup remains on the scale. Removing or replacing that cup, or starting
another cycle, discards the pending Bullseye.

In **Buzzer only** (and Scale first when the scale is not usable),
tare/retare sounds follow paddle/retare immediately and do not wait for a
Bluetooth round-trip. Shot **start** and **stop** always follow machine circuit on the
local buzzer whenever one is compiled in, including Scale only and Scale
priority with a connected scale. The scale timer may keep running briefly
after machine circuit opens; that does not delay the stop beep.

**Mute the Bookoo speaker** and **Scale volume** live under
[Scales](settings/scales.md). They change scale speaker behavior, not this
Alerts group.

## Example

Brew by weight finishes, machine circuit opens, but you left the paddle ON. The
paddle-off reminder repeats every 30 s for up to 15 minutes until you return
the paddle to OFF.

## Choose an output

Use **Buzzer only** for local cues with a compiled passive buzzer. Use
**Scale only** if the scale supports the requested sound. **Scale first**
falls back to the buzzer for unsupported/unavailable scale sounds.
Start/stop local cues are the exception described above.

If a sound is missing, check Sound alerts, the output mode, the reminder
setting or other individual sound controls, and [scale capabilities](settings/scales.md)
before changing wiring.
A scale can support tare/timer without supporting arbitrary beeps.
Custom RTTTL follows `name:defaults:notes`, for example
`done:d=8,o=5,b=120:c,e,g`; paste it with Bullseye on and it is checked as it saves.
The defaults section accepts only `d=` (note length, 1–64), `o=` (octave, 4–8)
and `b=` (tempo, 25–900 beats per minute); a tune with any other setting or an
out-of-range value is rejected instead of playing at the wrong speed or pitch.
Two identical notes in a row play as two separate notes, with a short break between them.

Related: [scale-loss backup](features/auto-to-manual.md),
[fast and slow shots](features/fast-and-slow-shots.md),
[Hardware](HARDWARE.md) (buzzer GPIO and LED).
