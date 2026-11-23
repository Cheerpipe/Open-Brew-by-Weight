# Drip wait

When the machine stops, coffee keeps dripping into the cup for a few seconds.
The drip wait decides how long the controller waits for those last drips
before it records the shot's yield. Machine-level, under
**Settings → Shared settings → Drip wait**, shared by every preset.

## When it applies

After the machine stops, the controller waits for the last drips before it
records the yield. That one settled weight is what Last Shot, history, Stats,
drip learning, the Bullseye melody and the scale-loss backup's recent shots
all use. The wait adapts to each shot:

1. It always lasts at least the **Minimum drip wait**.
2. After that, the yield is recorded once the weight has stopped rising for
   the **Settle time**. A slow drip simply takes a little longer.
3. If the weight is still changing at the **Maximum drip wait**, the yield is
   recorded then.

Lifting the cup ends the wait at any point, and the shot keeps the weight it
had already settled at (see below). Starting the next shot also ends it. If
the scale switches off or loses its connection during the wait, the shot
keeps the weight it had settled at, too.
History and Stats show how long each shot actually waited (**Drip wait**),
and the Web UI log says why the wait ended.

The final weight is the weight the cup settles at during that wait, not simply
the last reading. If you bump the cup, press on it, or rest something on the
scale before the delay ends, readings that the last drips could not explain
are left out, and the shot keeps the weight it had settled at. Two touches can
still be counted as coffee: one that starts within the first second after the
stop and is held, and a light press still there in the final second. Leave the
cup alone until the final weight appears. If you lift it off early, or start
the next shot, the shot keeps the weight it had already settled at; within
about the first second, before it settles, it records the weight at the moment
the machine stopped. A cup lifted after its weight settled still teaches the
drip correction; one lifted before, or a hold ended by the next shot, does not.

## Parameters

| Setting | Default | Range / notes | Effect |
| --- | --- | --- | --- |
| **Minimum drip wait (s)** | 3.0 s | 0–10 s | The yield is never recorded sooner than this after the stop. `0` records it right away, without waiting for the weight to settle. |
| **Settle time (s)** | 3.0 s | 1–10 s | After the minimum wait, the yield is recorded once the weight has stopped rising for this long. Normal scale flicker of a tenth of a gram does not restart it. |
| **Maximum drip wait (s)** | 20 s | 5–25 s, at least the minimum | The yield is recorded at this point even if the weight is still changing. |

If the scale's own timer lags behind the shot, use the
**Scale timer stop delay** under [Scales](scales.md#parameters) instead: it
only delays the scale's timer and never changes the drip wait.
