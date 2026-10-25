# Tare

Four controls distinguish cup-placement tare outside a brew, optional accessory
retare while idle, shot-start tare, and late-cup retare. Machine-level, under
**Settings → Shared settings → Tare**, except **Earliest weight stop**, which is
saved with each preset under **Settings → Presets → Time limits**.

Initial weight-stop protection is a separate brew setting; its range and
interactions are in [Cup protection](../features/cup-protection.md).

Together with [Cup detection](cup.md), this is the setting side of
[Tare and retare](../features/tare-retare.md) and
[Cup protection](../features/cup-protection.md).

## When it applies

Initial tare and timer commands can also run on timer-only shots with a usable
scale. Late retare and **Earliest weight stop** apply to automatic BBW, not timer-only or
manual no-scale cycles. Shot **Post-tare grace** is inactive when **Tare when the shot starts**
is off.

First-drop / first-flow detection still runs against the tare zero even while
the grace window is open. A small stream of coffee does not wait for the
scale to settle. The detector rejects typical cup jumps, finger taps and
vibration that shakes the reading up and down, and it still recognizes coffee
that arrived during a brief pause in the scale's readings. Weight alone cannot
identify every disturbance. An earlier first-drop detection
does not block a qualified late cup within the retare window.

Until first drops are detected, the late-cup window stays open past **Late
cup time** for as long as **Earliest weight stop** still blocks weight stops,
which covers machines that pre-infuse. For a cup placed after first drops and
after the retare window, stop and restart correctly; the controller does not
promise another automatic tare outside that window.

## Parameters

| Setting | Default | Range | Effect on the shot |
| --- | --- | --- | --- |
| **Tare when you place a cup** | ON | ON / OFF | Tare once on a new stable cup placement while idle, independently of shot-start tare, BBW, and cup protection. Never tares merely because a shot ends. |
| **Tare items added to a cup** | ON | ON / OFF | Before brewing, tare a stable added load on a cup already tared outside a brew, then retare its known removal while the cup remains. Requires **Tare when you place a cup**. |
| **Tare when the shot starts** | ON | ON / OFF | Tare at shot start when a usable scale supports it, including timer-only shots. |
| **Post-tare grace (s)** | 2 s | 0.5–10 s | After a tare (start or late-cup retare), wait this long for ~0 g before using weight for **stop/control**. If the shot-start tare has not zeroed a cup by then, the shot is measured from the weight before the tare. Raise it if your scale is slow to show zero. |
| **Tare a late cup** | ON | ON / OFF | Allow one qualified late-cup retare within the original window, even after first drop was detected. Requires automatic BBW and shot-start tare. |
| **Late cup time (s)** | 4 s | 0.5–10 s | Time after shot start to detect and retare a late-placed cup. Until first drops are detected, the window extends to **Earliest weight stop**. |
| **Bookoo combined command** | ON | ON / OFF | Use the scale’s combined tare + start-timer command at shot start. Needs tare at shot start. Idle tare never starts the timer. Bookoo only. |

All tare switches are shared machine settings, not preset values. Save changes
while idle. **Tare when you place a cup** defaults ON on first setup, factory
reset, and upgrade from a configuration without this field. A saved OFF value
survives reboot and later upgrades. Enabling it with a cup already present does
not tare that cup. Accessory retare defaults ON after installation or factory
reset; its saved OFF value survives reboot and firmware upgrades. Turning it on
for an already-tared cup takes effect with the next cup placement and
controller-confirmed tare.

## Outside a brew

After connection, let the controller observe a stable empty pan near zero,
then place the cup. A single absent sample cannot authorize idle tare.
The existing [Cup detection](cup.md) stability settings qualify the placement and the empty
reference. Once that reference is known, a
[qualified brief unload](cup.md#fast-replacement-outside-a-shot) followed by stable
placement also works without another stable-empty pause.
Either placement path triggers another tare, even if the cup contains coffee.
With accessory retare OFF, adding coffee or a spoon while the cup stays present
does not tare.

With **Tare items added to a cup** ON, place and let the cup
auto-tare first. Add the accessory before starting the shot, then wait for its
added weight to meet **Minimum cup weight** and the configured stability window.
The controller tares that addition once; the cup remains present and its
calculated load includes the accessory. If you then remove that accessory before
brewing, a matching stable weight drop triggers another tare while the cup stays
present. You can add the accessory again after that removal. A second addition
without an intervening known removal, or any change during or after its shot,
does not trigger this option. Set the accessory on the cup in one motion. It
is fine to press it into place or let it rock briefly: the controller waits up
to about three seconds for the weight to settle. Something that builds up
gradually (milk, syrup, coffee) is not tared. Neither is a load that keeps
growing after it first lands, or a second addition that starts while the first
is still on the scale. Because the scale reports weight rather than what was
added, a fast splash that stops within about a second can still look like an
accessory. Leave this option OFF if you put ingredients in the cup before
brewing. If the accessory is knocked before the tare is sent, the controller
tries again once its weight settles.
Disconnected, stale, uncertain, or interrupted readings cannot authorize it.

**Home → Cup → Accessory** shows what the controller knows about the item on
the cup:

| Shown | Meaning |
| --- | --- |
| **42.2 g** | An accessory was tared on this cup; its weight is shown. |
| **Absent** | No cup, or the cup is tared and ready for an accessory. |
| **Settling…** | An accessory being added or removed is waiting to settle. |
| **Not tracked - lift the cup and set it down again** | The controller is not following accessories for this cup, for example after a shot, a settings change, a scale reconnect, or a failed tare. |
| **Off** | **Tare items added to a cup** or **Tare when you place a cup** is OFF. |
| **Unknown** | No scale is connected, or its readings are not current. |

When the cup and accessory were placed together, the controller knows only their
combined weight. Removing one part cannot be identified reliably, so it does
not retare that partial drop. Remove everything, let the empty pan settle, and
place the cup again to resume automatic tare. If the separately measured cup
and accessory have nearly equal weights, their individual removal is also
ambiguous and does not trigger accessory retare. A failed or unconfirmed tare
may require taring the empty pan on the scale and reconnecting it before
automatic placement can resume.

Moving an empty scale and returning it near its previously stable zero does not
authorize an idle tare. Let it settle through the normal stability window;
**Cup tare** returns to **Ready for a cup**, including after positive and
negative fluctuations before the first cup has been weighed. Small stable
empty-pan offsets use the same near-zero range as at connection; recovery adds
no delay to the usual stability or cup-placement timing. If zero was already
confirmed before handling and readings remain continuous, a return near zero
allows the next stable cup to tare without waiting for the empty pan to settle
again. A positive return after a negative fluctuation can also qualify if its
increase from the previously confirmed zero is below **Minimum cup weight**.
That small reading does not send a tare; the next cup must complete its normal
stability window. Without that earlier zero or after a reading gap, let the empty pan
complete the stability window before placing the cup. A larger negative reading first
seen at boot/reconnect, without a preceding qualified zero and continuous unload,
requires an empty-pan tare on the scale, a reconnect, and stable zero; see [Cup detection](cup.md).

No shot or rinse may be running. A new placement can tare after a normal
shot stop while the paddle is still ON; the paddle still must be released before
another shot. Rinse, active shots, uncertain stop state, safety failures, and
maintenance exclude idle tare. An excluded placement is not replayed later.

A cup detected before tare remains present at 0 g and satisfies **Require cup
to start**. Other start protections still apply. Boot/reconnect loses presence
evidence. With a cup already reading a positive weight, remove it, let the empty
pan settle near zero, and replace it. If the Bookoo zeroed a cup already on its
pan at power-on, let that zero settle first, then remove the cup. A continuous,
stable negative reading can establish the empty reference; placing a cup with a
different weight can then trigger tare. A return near the original zero is
treated as possible empty-scale movement. For an equal-weight replacement, a
large removal rebound, a reading gap, or a negative reading first seen at
connection, tare the empty pan on the scale, reconnect it, wait for stable zero,
then place the cup.
The integrated protocols currently do not report a verifiable physical-button
tare event; a zero reading alone cannot distinguish that action from returning
to the previous displayed weight. **Tared** on Home describes the controller's
last known cup state, not proof that the physical button was or was not pressed.
If you press the scale's button after removing the cup, let the empty pan settle
at zero; the controller takes that zero as the new empty pan and the next cup
tares normally. The flip side: if a cup of exactly the same weight is set down
before the empty pan settles, it reads near that old zero and is taken for an
empty pan, so **Cup tare** waits for a cup. Lift it, let the pan settle, and
place it again. Accessory retare does not resolve this ambiguity.

An accepted physical shot or Quick rinse start takes priority over a pending
idle tare; use the normal paddle or button gesture without releasing and
retrying. A queued idle tare is canceled, while a scale write already in
progress may finish. A shot starts its internal timer and sends its normal
shot-start timer and tare command. The older idle result stays separate from
the new cycle and cannot trigger a late retare or replace its baseline.
No-scale, cup, maintenance, and safety protections still block starts normally.

Queued requests expire after 1 s. An executing write stays serialized until it
returns, and its result is cleaned up within the 1 s write allowance plus the
configured post-tare grace. Failed requests are not automatically retried on a
cup that remains present.
If an accessory is added or removed but returned to the previous zero before
its queued tare is written, that cancelled change can be tried again on the
same cup. A failed or unconfirmed scale write still needs fresh reference
evidence; merely returning to zero does not retry it.

Before an idle write starts, fresh readings must still satisfy the configured
minimum using the original direct/relative reference and the placement stability
window. A changed or stale placement cancels the queued request;
remove and replace the cup to authorize another attempt. An executing write
keeps its ownership until it returns, even if the cup is removed or settings
change.

Write success alone does not confirm the cup's zero reference. The controller
checks notification capture order against the write boundary, including zero
readings received during the write. Buffered pre-write readings cannot confirm
the effect. If the bounded settling period ends without it, a fresh unchanged
reference is retained; otherwise the reference becomes uncertain and cannot
satisfy **Require cup to start**. A later unambiguous lift/replacement or a new
connection can establish fresh evidence; a zero alone cannot reconstruct
physical motion hidden by simultaneous tare.
If the pan was not observed empty long enough after an unconfirmed tare, a
quickly replaced cup cannot establish that empty reference. Remove the cup and
let the empty pan settle; if its reading remains ambiguous, tare the empty pan
using the scale's button, reconnect it, and wait for stable zero before replacing the cup.

Tracked tares translate the known empty reference from the latest control-approved
reading captured immediately before the write, rather than the enqueue weight.
An idle request waits for control to approve any readings collected at the final
pre-write check, within its original expiry. Contradictory evidence cancels it.
Other tracked tares invalidate the anchor and cup mass when that evidence is
missing; they do not fabricate a new empty zero.

**Home → Cup → Cup tare** shows whether the empty pan must settle, the
controller is ready for a cup, the machine must turn off, tare is pending, or the
cup needs removal and replacement. An uncertain reference asks you to tare the
empty scale and reconnect it. **Stale**, **No sample**, and **Disconnected** take
precedence when scale data is unavailable. A smoothly displayed weight does not
prove a qualifying placement: a gap longer than **Max sample gap** also requires
fresh removal/placement evidence. After a successful tare, the message stays
**Tared** while that cup remains present. While the empty pan must settle, the
message also shows how far the reading is from the empty reference and, once
the reading has held steady there for a second or more, for how long; a reading
well above it that does not change usually means something is still on the pan.

The debug export includes `idleTare`: request/placement IDs, eligibility
and terminal reasons, reference confidence, qualifying weight range, capture
and command times, sequence boundary, dropped/rejected sample counts, and
`anchorDeltaG` (the idle reading minus the empty reference while it waits for
the pan to settle, otherwise `null`) with `anchorWaitMs` (how long that steady
off-reference reading has lasted, otherwise 0). The scale profiler records one
`CUP_ANCHOR_REJECTED` event per steady reading rejected as off the empty
reference, with the placement ID and the offset in centigrams.
These fields come from the control snapshot. `effect_unconfirmed`, `unstable`,
`stale_sample`, `sample_gap`, and `removed` distinguish common incomplete
attempts. `qualificationMinG`/`qualificationMaxG` describe the observed window,
not configured minimum/maximum cup mass. `requestPlacementId` identifies the
originating placement even after a different cup replaces it. An accessory
request released as `unstable` before its write can be requested again once a
new stable window qualifies; no periodic retry is performed.

The Home stream additionally exposes `cupPresence.idleTare`
as a presentation code: `empty`, `ready`, `pending`, `tared`, `remove`,
`retry`, `uncertain`, or the existing eligibility reason, and
`cupPresence.anchorDeltaG` with the same meaning as in the debug export, plus
`cupPresence.anchorWaitS`, the same wait in whole seconds, plus
`cupPresence.accessory` (`off`, `unknown`, `absent`, `not_tracked`, `settling`,
or `present`) and `cupPresence.accessoryG` (grams while `present`, otherwise
null). They are derived from the
same control snapshot; it neither authorizes commands nor changes the separate
integration API. Clients must also check scale availability and stream state.

## Example

An earlier first-drop detection does not prevent a qualified cup placement
from triggering retare. The earlier time and chart marker are replaced only
after successful retare and a fresh reading near zero within **Post-tare grace**.
If no new first drop follows, its time and average flow remain unavailable.
Failure or an unconfirmed zero preserves the earlier record. **Late cup**
still reports successful command completion, so it can appear without a
confirmed zero. The retare deadline, shot timer, and stop protections stay
the same. See [Tare and retare](../features/tare-retare.md#automatic-late-cup-retare)
for detection limits and announcement behavior.

Shot-start tare is on, grace 2 s, retare window 4 s. The shot starts, the
scale tares, then you place the cup at 2 s. A second tare fires. Weight stop
still waits for the earliest weight stop time (default 12 s) so the cup weight cannot cut
the shot.

Related: [Tare and retare](../features/tare-retare.md),
[Cup protection](../features/cup-protection.md), [Cup detection](cup.md),
[Scales](scales.md).
