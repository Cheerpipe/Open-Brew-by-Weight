# Drip wait

When the machine stops, coffee keeps dripping into the cup for a few seconds.
The drip wait decides how long the controller waits for those last drips
before it records the shot's yield. Machine-level, under
**Settings → Shared settings → Drip wait**, shared by every preset.

The [drip correction](../features/brew-by-weight.md) learns from that recorded
yield, so a drip wait that ends before the drips settle teaches it the wrong amount. Its
**Adjust the drip wait** link, under Drip compensation, opens this section.

## When it applies

After the machine stops, the controller waits for the last drips before it
records the yield. That one settled weight is what Last Shot, history, Stats,
drip learning, the Bullseye melody and the scale-loss backup's recent shots
all use. The last drips rarely follow a schedule, so the longer the cup stays
on the scale, the truer its final weight:

1. The wait always lasts at least the **Minimum drip wait**.
2. After that, each time the weight stops rising for the **Settle time**, the
   controller notes a settled weight in the Web UI log, and keeps watching. A
   drip that adds one more tenth of a gram a few seconds later still counts.
3. **Lifting the cup records the yield**: the weight the cup had at that
   moment, once the readings had time to steady (about the first second
   after the stop). If you leave the cup in place, the yield is recorded at
   the **Maximum drip wait**.

Starting the next shot also ends the wait. If the scale switches off or loses
its connection during the wait, the shot keeps the weight it had settled at,
too. History and Stats show how long each shot actually waited (**Drip
wait**), and the Web UI log says why the wait ended. Home shows **Last
drips…** until the yield is recorded, so lift the cup when you are ready, or
wait for the maximum.

The final weight is the weight the cup settles at during that wait, not simply
the last reading. If you bump the cup, press on it, or rest something on the
scale before the delay ends, readings that the last drips could not explain
are left out, and the shot keeps the weight it had settled at. Two touches can
still be counted as coffee: one that starts within the first second after the
stop and is held, and a light press still there in the final second. Leave the
cup alone until you lift it. Within about the first second after the stop,
before the readings steady, lifting the cup records the weight at the moment
the machine stopped. A cup lifted after that still teaches the drip
correction, and so does a wait ended by the next shot once the weight had
settled at least once; one ended earlier does not.

## Parameters

| Setting | Default | Range / notes | Effect |
| --- | --- | --- | --- |
| **Minimum drip wait (s)** | 3.0 s | 0–10 s | The yield is never recorded sooner than this after the stop. `0` records it right away, without waiting for the weight to settle. |
| **Settle time (s)** | 3.0 s | 1–10 s | After the minimum wait, the weight counts as settled once it has stopped rising for this long. A reading even a tenth of a gram higher starts it again, since a still cup holds its weight; a reading lower does not. The wait goes on until you lift the cup. |
| **Maximum drip wait (s)** | 20 s | 5–25 s, at least the minimum | If the cup is still on the scale, the yield is recorded at this point, settled or not. |

If the scale's own timer lags behind the shot, use the
**Scale timer stop delay** under [Scales](scales.md#parameters) instead: it
only delays the scale's timer and never changes the drip wait.
