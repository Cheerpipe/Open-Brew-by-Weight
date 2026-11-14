# Scale guard

Protection for attempts to use Brew by weight without a usable scale, shared
by every preset. Configure it under **Settings → Shared settings → Scale guard**.
The same switch in the header's [guard panel](../features/brew-by-weight.md#change-how-shots-stop-for-now)
turns it off or back on for now, without saving.

It applies only while **Brew by weight** is on. With BBW off, shots and rinses
remain manual regardless of this setting. A scale is usable only when its link
is available and its weight stream is fresh.

On Linea Micra, a recognized standby wake gesture is not a shot or rinse
attempt. It mirrors the paddle only, so this protection is neither evaluated
nor disarmed. See [Linea Micra](linea-micra.md#recognize-paddle-wake-gestures).

## Modes

| Mode | Shot without a scale | Rinse without a scale | Cooldown |
| --- | --- | --- | --- |
| **Brew manually** (default) | Allowed | Allowed | Not used |
| **Warn once, then allow** | First attempt is blocked and alerts; later attempts are manual | An Armed rinse runs, because **Rinse without a scale** is on by default. Turn it off and an Armed rinse does not run water; it uses up that warning so the next brew can be manual | Protection returns after the configured delay |
| **Require a scale** | Blocked unless temporarily allowed with the physical gesture | Blocked unless temporarily allowed with the physical gesture | Protection returns after the configured delay |

In **Require a scale**, shots and rinses cannot start until the scale is usable.
If the scale is unavailable and you need to brew, start with the activator OFF,
then switch it ON and OFF three times within two seconds. The scale-connected
melody confirms that manual brewing is temporarily available. A fresh activation
is required after the gesture, so its final movement never starts a shot. If the
scale reconnects while the activator is held, release it before starting again.

**Warning returns after (min)** applies to **Warn once, then allow** and the
temporary **Require a scale** override. It defaults to 60 minutes and accepts
5–240 minutes. Boot and scale reconnect re-arm protection immediately.

**Rinse without a scale** is on by default. It applies only to **Warn once,
then allow**. With it on, a rinse while the missing-scale warning is Armed
still runs. You hear the missing-scale warning when you start the gesture,
then the rinse sound if you finish it. With it off, that rinse does not run
water. The gesture still uses up that warning, so the next brew can be manual.
Home **Start rinse** follows the same choice. **Require a scale** still blocks
rinse unless you complete the temporary physical override.

While **By weight** is selected and no usable scale is connected, a notice at
the top of Home says what your next shot will do. While protection is Armed,
it warns that the next shot will be held back, or, in **Require a scale**, that
you need to connect a scale to start. Once the warning is used up, or while
manual brewing is temporarily allowed, the notice reminds you to stop the shot
with the paddle instead.

When a shot runs without a scale, Home turns the shot card into a timer: the
large number counts the seconds of the pour, the target weight stays out of the
way, and a **No scale** badge replaces the brew mode. The weight chart and
target error have nothing to measure, so they stay quiet. About half a minute
after the shot ends, the card goes back to showing the last shot you poured
with a scale. Shots made with a connected scale keep the usual weight card
unchanged. While the timer is running, its tenths advance smoothly between
updates from the controller. The final time comes from the controller. If
updates stop arriving for 1.5 seconds, the timer freezes and the card shows that
its data is out of date; it resumes from the controller's time when updates return.
When Home receives a paddle OFF update or confirms that the cycle has stopped,
the timer stops and waits for the controller's final time. If the two agree to
within a tenth of a second, the number simply settles on the final time. A
larger difference fades the number and its seconds unit out over half a second,
swaps in the final time while hidden, and fades it back in over another half
second. Browsers set to reduce motion show the final time without the fade.

Home Assistant presents this three-mode policy as one switch. Turning it OFF
sets **Brew manually**. Turning it ON restores the last non-off mode that
Home Assistant observed, or **Warn once, then allow** when none is known. The
switch is unavailable during an active cycle; changing it does not alter the
active recipe.

The optional **Manual without scale (BBW on)** alert sounds once per distinct
blocked attempt; holding the activator does not repeat it continuously.
Each blocked paddle attempt also appears in **History** as **No scale guard
aborted** when you release the paddle. Repeated attempts in **Require a scale**
each get an entry. A rinse that actually runs is recorded as a rinse instead.

## Examples

- **Warn once, then allow:** with BBW on and no scale, the first shot attempt
  warns and stays blocked. Release the activator, then make a fresh attempt
  for manual brewing; protection returns after the cooldown. A rinse while
  Armed uses up the warning without running water unless **Allow rinse while
  Armed** is on.
- **Require a scale:** repeated ordinary attempts remain blocked. Either
  restore a fresh scale stream or deliberately use the temporary physical
  override above, then start with a new gesture.
- **BBW off:** this guard does not block the manual workflow. This does not
  create weight control or guarantee a stop pulse on a button-only machine.

Related: [Quick rinse](quick-rinse.md), [Momentary](momentary.md),
[Brew by weight](../features/brew-by-weight.md), [Alerts](../alerts.md).
