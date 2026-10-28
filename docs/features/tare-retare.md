# Tare and retare

Three main settings control automatic zeroing: **Tare when you place a cup**
on a new cup placement, **Tare when the shot starts**, and **Late-cup retare
during a shot** if you place the cup after the shot starts. An optional fourth
switch, **Tare items added to a cup**, extends only the
outside-brew behavior.

All four are **on by default**. Late retare also requires shot-start tare.

## Automatic tare outside a brew

When no shot or rinse is running, a qualified stable empty pan followed by a stable
placement triggers one tare using the same cup detector as late retare. It
does not start the timer. A cup that remains on the scale after a shot never
triggers this tare, even after the beverage becomes stable. Removing and
replacing that full cup does trigger it.

**Home → Cup → Cup tare** shows readiness and what to do when tare cannot
proceed. If you switch on with a cup already showing a positive weight, remove
it, let the empty scale settle near zero, then replace it. If the cup was already
tared to zero before connecting, let the controller see its steady zero, then
remove it. A continuous stable negative reading can become the empty reference;
placing a **different-weight** cup then triggers tare. If readings were lost, the
replacement reads near the original zero, or the scale started substantially
negative, tare the empty scale using its button, reconnect it, wait for stable zero, then place the cup.
Weight readings alone cannot distinguish an equal-weight replacement from the
scale being moved and returned.
If you move the empty scale after it was ready, leave it still near its original
zero. Once the normal stability window completes, **Cup tare** returns to
**Ready for a cup** without sending a tare. A rebound away from that known zero
can remain ambiguous; tare the empty pan on the scale and reconnect it in that case.
Before the first cup has been weighed, recovery accepts the same small empty-pan
offsets as at connection, even if handling briefly looked like a cup. It uses
the normal stability window and adds no delay to the next cup's tare.
If zero was already confirmed and readings remain uninterrupted, returning the
empty scale near zero lets the next stable cup tare without another empty-pan
pause. After a negative handling fluctuation, a positive return below the
minimum cup load also works using that same confirmed zero. For example, with
a 10 g minimum and a previously confirmed 0 g, returning at 7.2 g before placing
the cup does not require another empty-pan pause. The return itself does not
send a tare. The cup must still complete its normal stability window.

Once an empty reference is known, two consecutive fresh readings showing
near-total unloading can authorize a stable replacement without another stable
empty-pan pause. This works for lighter, equal, and heavier replacements,
including negative tare offsets. An empty-pan rebound never counts as a cup;
the detector reuses the known reference, not the removal minimum. Without that
reference or sufficient unloading evidence, remove the cup, let the empty pan
settle, and replace it. A swap hidden between notifications cannot be guaranteed.
See [Cup detection settings](../settings/cup.md#fast-replacement-outside-a-shot) for the
unloading and stability requirements. A stable load change alone does not
trigger tare with the accessory option OFF.

If you use a chilling bar or another accessory, keep the accessory option on
under **Tare when you place a cup**. Let the cup auto-tare to zero, then
set the accessory on in one motion before brewing; pressing it into place or
letting it rock briefly is fine. The controller tares one such addition per cup
placement and shows it under **Home → Cup → Accessory**. Removing that known
accessory before brewing causes another tare after its weight settles, while
the cup remains present. If cup and accessory arrived together, or their
separate weights are too similar, remove everything and let the empty pan settle
before placing the cup again. The same weight change during a shot follows the
existing shot behavior, and a full cup
settling after the shot does not trigger accessory retare. A gradual pour is
not tared, but a fast splash that stops within about a second is
indistinguishable from an accessory, so leave the option OFF if that could
happen during preparation. See [Tare settings](../settings/tare.md#outside-a-brew)
for prerequisites and limits.

After the first cup placement, the empty reference remains fixed across larger
disturbances. Let the empty pan settle before the next placement. Small, stable
negative shifts before the first cup can qualify as an empty pan.
Larger initial negative offsets without a preceding qualified zero cannot
authorize relative placement; recovery is described in [Cup detection settings](../settings/cup.md).

A known tared cup remains present at 0 g, so **Require cup to start** accepts
it. If shot-start tare is enabled, starting the next shot still performs its
normal tare. If that start races the idle tare, the accepted paddle or button
gesture starts immediately: queued idle work is canceled, and a write already
in progress may finish without entering the new shot. You do not need to release
and retry. Other start protections remain authoritative. See
[Tare settings](../settings/tare.md#outside-a-brew) for idle prerequisites,
pending-command behavior, and reconnect/physical-tare limitations.

Placement stability limits the total spread across the observed window, and
fresh contradictory readings cancel an idle tare before it is sent, including
while the scale is waiting between commands. A zero seen during that wait does
not confirm that the cup was tared.
Replacing a cup with a lighter one can leave a negative net reading; that
occupied reference stays valid until an actual further lift or tare changes it.
Valid negative removal readings from a previously accepted heavy load still
reach the cup detector; this does not expand the placement or brew weight limits.
If a much lighter replacement remains below the supported −500 g net-reading
limit, tare the empty pan on the scale, reconnect it, wait for stable zero, then place it.

Observed removal during drip analysis preserves the previous shot's captured
last-known weight and skips post-drip learning, even with idle tare disabled.
A single credible near-empty reading can discard that optional analysis;
it cannot authorize a replacement tare. A completely unsampled swap remains
indistinguishable from some additions to the same cup.

## Automatic tare at start

When an automatic shot begins with a usable scale, the firmware sends tare
so the empty pan (or cup already on the scale) reads ~0 g. After that,
**Post-tare grace** waits for the reading to settle before weight is used
to stop or control the shot. If a cup was on the scale when the tare was
sent, the new zero is accepted only once the reading has clearly dropped
from the cup's weight, so a late reading that still shows the cup is never
mistaken for zero. First-drop detection still runs against that tare zero.

If that tare does not take effect in time—the scale rejects it, or still
shows the cup when post-tare grace ends—the shot keeps going and is measured
from the weight shown just before the tare. The cup's own weight then cannot
stop the shot or be mistaken for first drops. Once the controller has
recognized the cup on the scale (after a moment of steady readings), lifting
it still stops the shot.

Some scales accept the tare but only reach zero a moment after the grace
period. If that zero arrives within about one more grace period, the
controller recognizes it as the tare it asked for: it keeps the coffee
already weighed and carries on to your target as usual. A sudden drop to zero
looks exactly like lifting the cup, though, so if the controller has already
recognized the cup and **Stop if cup is removed** is on, the shot stops as it
would for a lift. If your scale is often slow to show zero, raise
**Post-tare grace**.

A cup the controller has not recognized yet looks the same when you lift it
during that moment: the reading drops to zero, and the controller takes it for
the late tare. When you put the same cup back, the controller sees the cup and
its coffee return, measures the shot from the cup again and carries on to your
target; the Web UI log says "late scale tare was a cup lift". Because the
cup was off the scale for a while, that shot does not teach drip learning.

Coffee in a cup can never make the scale read below zero. If, during a shot,
the reading stays below minus your **Minimum cup weight** (or minus 10 g,
whichever is lower), the controller stops trusting the weight after about
four readings for a light cup, or after up to about 2.5 seconds and three more
readings when a heavy cup (around 250 g) was lifted, and the [scale-loss backup](auto-to-manual.md) finishes the shot by
time, instead of letting it run until the max shot time.

## Automatic late-cup retare

If the cup is not on the scale yet, you can place it after the shot has
started. Cup-presence detection watches for a stable load at or above
**Minimum cup weight** (`minimumCupWeightG`, default **10 g**; cardboard
cups around 12–20 g use this same setting). When a cup is **placed** inside
the **retare window** (default **4 s**), the firmware tares again
once—automatically—without restarting the shot timer. The cup must arrive as a
step: coffee gradually building up on an empty pan is not mistaken for a late
cup and is never tared away.

An earlier first-drop detection does not block a qualified late cup, whether
it is a light cardboard cup or a heavier ceramic cup. Weight readings alone
cannot always distinguish coffee from pressure or handling. A stable load
that meets the cup-placement rules can trigger retare within the original
window; detecting first drop does not shorten that window. If no first drop
has been detected yet, the window stays open past **Late cup time** until
**Earliest weight stop** ends, so a pre-infusing machine still gets its late
cup tared.

The earlier first-drop time stays recorded until the retare succeeds and a
fresh reading confirms the new zero within post-tare grace. Then its time and
chart marker are cleared, and later coffee can establish a replacement. If
no new first drop is detected, first-drop time and average flow remain
unavailable. A failed or unconfirmed retare keeps the earlier first drop.
An already played first-drop sound or sent webhook cannot be recalled, and
a replacement does not repeat either announcement.

After the late tare, post-tare grace runs again so the empty-cup weight cannot
cut the shot; [Cup protection](cup-protection.md) still blocks weight stop for the
full earliest-weight-stop period. The tare moment is stored with the recorded shot
and shown as **Late cup** on the shot cards, with a scale marker at that
instant on the shot chart. The time is measured from the machine circuit
closing, when the tare command completes successfully. Failed commands and
tares completed after the shot ends do not add a tare time. A later Bluetooth
disconnect does not erase a successful tare already recorded for the shot.
Tare time reports command completion; it can be present even when the new
zero was not confirmed and the earlier first-drop time was retained.
Stopping the shot cancels a queued late tare that has not yet been sent.

## When it applies

Initial tare can also run on a timer-only shot with a usable scale. Late retare
requires automatic BBW; timer-only (BBW off) and manual no-scale cycles skip it
and the earliest weight stop. Shot post-tare grace is inactive when **Tare when the shot starts** is off.

## Parameters

Configured under **Settings → Shared settings → Tare** (and **Cup detection** for
presence thresholds). Full tables: [Tare](../settings/tare.md),
[Cup detection](../settings/cup.md).

## Example

Cup placed while idle: placement tare → cup reads 0 g and remains present →
shot starts → shot-start tare → brew continues.

Cup placed two seconds after paddle ON: start tare runs, then cup detection
fires a second automatic tare inside the 4 s window. The shot timer does
not restart; weight stop stays blocked until the earliest weight stop time.

If you place a cup after the 4 s default window, automatic retare is no longer
available for that placement. If **Tare when the shot starts** is off, prepare the scale's
zero yourself; do not assume post-tare settling protection is active.
[Require cup to start](cup-protection.md) is an alternative when you always
want the cup in place before brewing.

Related: [Cup protection](cup-protection.md), [Brew by weight](brew-by-weight.md),
[Tare](../settings/tare.md), [Cup detection](../settings/cup.md).
