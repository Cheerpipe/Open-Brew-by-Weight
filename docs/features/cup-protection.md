# Cup protection

Cup protections handle missing cups, cup removal, and brief bumps. They apply
to automatic brew-by-weight shots. The cup protection switches and **Avoid
accidental touch** are shared by every preset, in **Settings → Shared
settings → Cup protection**; detection thresholds are in **Cup detection** and
tare timing in **Tare** of the same section. **Earliest weight stop** is saved
with each preset under **Settings → Presets → Time limits**.

## Choose the behavior

| Control | Factory value | What it does |
| --- | --- | --- |
| **Cup protection** | On | Enables the cup-start/removal protections. You can also turn it off for now from the header's [guard panel](brew-by-weight.md#change-how-shots-stop-for-now). |
| **Stop if cup is removed** | On | Requests a stop when a cup is detected as lifted after tare. This includes a cup that was already tared to 0 g before the scale connected: once the shot-start tare has settled near zero, a confirmed drop below the removed threshold stops the shot. A short tilt of a cup whose weight is known does not count, and neither does a slow drift. Requires cup protection. |
| **Require cup to start** | Off | Blocks a start without a detected cup. Requires cup protection. If the scale was already tared before connecting, place the cup after connection so it can be detected. |
| **Avoid accidental touch** | On | Ignores brief bumps — a hand steadying the cup or repositioning a chilling ball — so they cannot trigger a weight stop, including fast and slow shot handling. A light tap and a heavy hand on the cup are treated the same way: weight stops wait while the touch lasts, a load that stays for more than a couple of seconds below the cut line is counted as weight so the cup cannot overflow, and a load that stays above the cut line can end the shot only through the [touch backup](brew-by-weight.md#when-touch-protection-delays-a-stop). Shared by every preset; also in the header's [guard panel](brew-by-weight.md#change-how-shots-stop-for-now) for temporary changes. |
| **Earliest weight stop (s)** | 12 s | Blocks automatic weight stop at the beginning; first drops do not end the window. Maximum 30 s; minimum is enabled retare window + 3 s (3 s when retare is off). Must fit within Max shot time. |

A cup-start refusal holds the relay open for that attempt. Release the physical
activator, correct the cup/scale condition, then activate again. A blocked
held button is not forwarded halfway through the hold.

When cup removal stops a shot, Home's cup-protection status shows that the
shot was aborted after brewing ends.

The mark on the cup icon in **Equipment → Cup** on Home tells you whether a
shot can start. A green check means a cup is on the scale, and its weight shows
on the right. An amber exclamation mark means no cup is detected, or the scale
is off and the row reminds you it **Needs the scale**, but a shot can still
start, so check before brewing. A red cross means **Require cup to start** will
block the shot, and the row reads **Absent - cup required**. An amber mark
with **Not tared: lift it and set it back** means a cup is on the scale but
was not tared automatically; lift it and set it back before brewing (see
[tare outside a brew](../settings/tare.md#outside-a-brew)).

On Linea Micra, a recognized standby wake gesture is not a brew attempt, so
**Require cup to start** does not block it or emit a missing-cup alert. See
[Linea Micra](../settings/linea-micra.md#recognize-paddle-wake-gestures).

A detected cup that was tared to 0 g remains **present** and passes **Require
cup to start**, including after idle tare and with shot-start tare disabled.
The guard uses cup presence, not a positive-weight threshold. An empty pan at
zero does not satisfy it. Connection loss invalidates that presence evidence;
see [idle tare limitations](../settings/tare.md#outside-a-brew).

Stable negative readings at boot do not establish an empty reference for relative
cup detection. With an empty pan, perform a firmware diagnostic tare, wait for
stable zero, then place the cup. After moving an already empty scale, let it
settle back at its original reference. Small residual errors alone do not count
as a cup; the configured minimum added mass still applies.

## How timing fits together

At shot start, the controller can tare immediately. If you place the cup within
the retare window, a second tare can run once without restarting the timer.
After each tare, a settle interval protects the weight readings. Independently,
**Earliest weight stop** delays weight-based stopping from the beginning of the shot.

Use [Tare settings](../settings/tare.md) for timing values and
[Cup detection settings](../settings/cup.md) for detection thresholds. Timer-only and
manual no-scale cycles skip late retare and the initial earliest-weight-stop period.

## Examples

- **Cup already in place:** start tare runs; a stable cup does not cause another
  tare just because it remains on the pan.
- **Cup placed at 2 s:** with the default 4 s retare window, stable placement
  triggers one retare. Weight stop remains blocked until the 12 s protection
  window ends.
- **Cup placed after the retare window, before any coffee:** on a machine that
  pre-infuses, the first drops can come later than the retare window. Until
  first drops are detected, a cup placed during the earliest-weight-stop
  period still triggers the one late retare.
- **Cup placed after first drops and after the retare window:** no automatic
  late tare is promised. Stop and restart with the cup correctly placed rather
  than interpreting its mass as coffee.
- **Cup lifted during brewing:** with cup protection and removal stop enabled,
  the controller requests stop. The physical stop mechanism depends on the
  [machine type](../../README.md#machine-types).

Do not press the scale with a finger to cancel a shot: touch protection can
ignore that load. Use the physical stop action for your
[paddle mode](../settings/paddle.md) or [button](../settings/momentary.md).

Related: [BBW](brew-by-weight.md), [tare and retare](tare-retare.md).
