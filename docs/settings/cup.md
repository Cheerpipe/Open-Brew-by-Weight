# Cup detection

Thresholds that decide whether a cup is **on the scale** or **lifted**. Used
by idle cup-placement tare, late-cup retare, and shot protection. Shared by
every preset, under **Settings → Shared settings → Cup detection**.

How those detections protect the shot is explained in
[Cup protection](../features/cup-protection.md). The cup protection
switches that act on these detections are also shared by every preset.

## When it applies

While you set this up, put your usual cup on the scale and watch the reading:
if the cup weighs less than **Minimum cup weight**, Settings shows a hint with
the measured weight — lower the minimum below it, or cup detection and late
tare will not recognize that cup.

Detection runs while idle and during brewing. A stable increase from the last
qualified absent reading, at or above the minimum cup weight, counts as **placed**.
A confirmed reading at or below the removed threshold counts as **lifted** when
it arrives as a drop: at least the threshold's size (or half **Minimum cup
weight**, if smaller) below the previous reading. A slow drift across the
threshold does not count. During a shot, if the cup's weight is known, a dip of
less than half of it—a tilted cup, for example—must last at least 0.4 s; a full
lift still counts after two readings. Before the first drops of coffee, such a
lasting dip is not a lift: it usually means a hand was steadying the cup when
the scale tared, so the shot keeps going and measures from the new level. A
full lift from there still stops it. Placement also requires a short run of
stable samples.
After a known tare, a cup at 0 g stays present; **Require cup to start** does
not require positive net weight. Replacement into a negative tare offset can
also count as placed. The same stability settings qualify initial absence and
placement; a known reference also supports the fast idle path below. A removal
undershoot followed by an empty-pan rebound does not count as another placement.
Without an absent baseline at boot, a stable absolute reading above the minimum
can establish presence, but cannot establish cup mass.

Once a cup has been weighed, the empty reference changes only after that cup
is lifted: the pan's next stable reading becomes the new reference, provided
it is less than **Minimum cup weight** above the previous one. This keeps
automatic tare working when the pan settles a few grams away from where it was,
or when the cup was tared with coffee in it. A higher stable reading could
still be the pan with something on it, so the controller keeps waiting until
the pan is cleared. While no cup is detected, a stable reading below the
reference also replaces it, since nothing can be on a pan that reads less than
empty. Outside a shot, a stable reading near zero replaces a reference far from
zero, which is what pressing the scale's own tare button on an empty pan
produces. Before the first cup is weighed, a
small empty-pan offset can be accepted after stable readings; it must stay
within 5 g of zero or half **Minimum cup weight**, whichever is smaller.
Several small shifts cannot add up to move this band farther from zero.
Larger movement cannot redefine zero: 0 → −20 → 0.1 g does not place a cup.
After moving the scale, an empty reading inside that same band restores
**Ready for a cup** through the normal stability window, without sending a
tare. This also works if handling briefly looked like a cup before the empty
pan was first recognized. There is no extra settling delay, and placing the
cup afterward uses the usual placement timing. If the empty reference is unknown
and a removal reading rebounds away from this band by a full minimum cup weight, the controller cannot
tell empty-pan movement from a new cup; tare the empty pan on the scale,
reconnect it, wait for stable zero, then place the cup.
Stability tolerance bounds sample spread; it is neither a minimum placement mass
nor a threshold that identifies a different cup.

Before the first cup, a previously confirmed zero can also be reused after
handling: if readings remain continuous and the scale returns near zero, the
next stable cup can tare without another empty-pan pause. After a negative
handling fluctuation, a positive reading less than **Minimum cup weight** above that
known zero also allows this recovery. It does not tare the empty pan or change
the known zero; the cup must still meet the normal stability requirements.
A missing initial zero or interrupted readings still require the empty pan to
settle first.

At boot/reconnect, initial empty-reference acquisition accepts a stable reading
near zero, within 5 g or half **Minimum cup weight**, whichever is smaller. If a
Bookoo zeroed an initial load at power-on, removing
it can reveal a negative empty reading. After a qualified zero, a continuous
downward change of at least **Minimum cup weight** followed by stable negative
readings can establish that new reference. A larger negative offset first seen at
connection or a reading gap cannot establish it. A return near the original
zero restores readiness after settling, but cannot identify an equal-weight
replacement; tare the empty pan on the scale, reconnect it, and let zero settle before
placing a cup. Weight alone cannot distinguish every sustained external force
from a real cup.

## Fast replacement outside a shot

With a trusted empty reference and no pending/uncertain tare, two consecutive
fresh readings at or below that reference plus a small residual band qualify
near-total unloading. The band is the smallest of **Placement tolerance**, 0.5 g,
and half **Minimum cup weight**. Both readings must fit **Max sample gap** and
the one-second removal confirmation window. A stable replacement at least the
minimum cup mass above the reference can then qualify without waiting for
stable emptiness. This also updates presence and cup weight with idle tare OFF.

Sample loss, invalidity, configuration/reference changes, and connection changes
discard the short-unload evidence. An empty rebound, partial lift that stays
above the band, coffee/spoon addition, or a different stable occupied weight
alone cannot authorize another tare. A fully supported cup can look like a real
removal; a swap entirely between notifications can be invisible. One unload
sample is insufficient for replacement tare. In-shot detection is unchanged.

## Displayed cup weight

**Home → Cup** shows presence under **Status**, approximate cup weight under
**Weight**, and a tared accessory under **Accessory** (see
[Tare settings](tare.md#outside-a-brew)). **Diagnostic → Scale → Cup weight**
shows the same cup weight.
When no scale is connected, **Status** shows **Unknown**. Once a scale is
connected, it shows **Present** or **Absent** according to cup detection, and
**Weight** shows **Absent** when there is no cup.
The presence state machine records the qualified empty reference while `ABSENT`
and the stable reading at the next confirmed `PRESENT` transition.
Cup weight is the difference: **stable present reading − qualified empty reference**.
The records retain their qualification times; the calculated mass belongs to
that placement. A fast replacement uses the trusted translated empty anchor,
without inventing a newly observed stable-empty record. It appears even with
automatic tare disabled.

For example, place a 300 g cup on a scale reading 0 g: the UI shows **≈ 300.0 g**.
After firmware tare, the live reading is 0 g and cup weight stays 300 g. Remove
the cup and let the scale settle at −300 g. A 350 g replacement reads 50 g;
the UI shows **≈ 350.0 g**, from **50 − (−300)**. Further successful firmware
tares and coffee additions preserve that mass, except an enabled accessory
retare updates it to include the added accessory, which **Accessory** also shows
on its own. It represents the load added
at placement, so an empty-cup interpretation assumes the cup was empty then.

The absent baseline uses the same sample count, whole-window tolerance, maximum
gap, and minimum stable time as placement. Once qualified, that reference survives
intermediate readings while a cup is being placed (for example, 0 → 5 → 300 g).
A transient removal minimum is not a baseline. Successful tracked tares translate
the empty reference using the latest control-approved reading at the write
boundary, without changing the previous placement's recorded mass. Unvalidated
pre-write evidence defers an idle write; other tracked tares discard that anchor. Lost sample evidence requires fresh
qualification but does not permit the reference to drift to a different plateau.
Without a reliable empty reference
before placement, the UI shows **—**, including when booting with a cup
already loaded. Removal, stale or invalid samples, excessive sample gaps,
connection changes, lost evidence, and uncertain tares clear the value.
Acquisition requires stable absence or a newly qualified unload with a trusted
anchor, followed by placement; tare alone cannot recover missing mass.
Nothing is persisted across restarts.

**Home → Cup → Cup tare** explains readiness separately from presence and
mass. A cup already tared to zero when the controller connected can appear
absent. If the controller first sees stable zero, then its continuous removal
produces a stable negative reading, a different-weight replacement can be
detected. An equal-weight replacement returns near the original zero and cannot
be distinguished from moving an empty scale; tare the empty pan on the scale,
reconnect it, wait for stable zero, and replace it. A positive-weight cup at
startup still needs stable empty-pan removal and replacement. Physical-button
tare does not restore tracked reference history.

The controller cannot verify a physical-button tare until the scale reconnects
and establishes a new empty reference. Older firmware payloads and unavailable
readings show **—**. Diagnostic API tare/combined commands invalidate this value. A successful
command rebases a detected cup to zero so its next lift remains detectable;
a failed command leaves presence uncertain and cannot satisfy **Require cup to
start**. Remove the cup, wait for stable absence, and replace it to acquire the
weight again. Pending idle placements affected by a diagnostic tare are cancelled.
Invalid/out-of-range readings and readings older than one second, future-dated
or out of order cannot qualify cup
placement or removal, including during a shot; they break the stability streak.
The stable absent reference is shared by placement detection and mass calculation.
The calculated mass itself does not control brewing. The optional accessory
retare requires a known cup reference but uses the added live weight, not the
calculated mass, to qualify its one outside-shot tare.

## In the Diagnostic log

Every cup decision leaves a line in the Web UI log with the weight the scale
showed at that moment: "cup placed", "cup lifted", or "cup kept: settled dip is
the new zero" when letting go of a steadied cup does not count as a lift. A
stopped shot reads "shot stopped: cup removed" with the weight and how long
after the start it happened. The shot-start tare reports "scale tare confirmed"
with the reading it accepted and how long the scale took, or a timeout line
saying how long it waited and whether the shot is being measured from the
weight before the tare (the software zero). Together these lines let you tell a
real lift from a tare or a hand on the cup after the fact.

## Parameters

| Setting | Default | Range / notes | Effect on the shot |
| --- | --- | --- | --- |
| **Minimum cup weight (g)** | 10 g | 1–500 g | Stable load that counts as a cup being placed. Shared detection threshold used by first-flow and retare. |
| **Cup-removed threshold (g)** | −3 g | −50 to −0.1 g | A confirmed drop to at or below this means the cup was lifted. |
| **Placement samples** | 3 | 2–10 | Number of stable samples required before cup-present. |
| **Placement tolerance (g)** | 2.0 g | 0.1–20 g | Maximum difference between the lowest and highest reading across the qualifying window, including while idle tare is queued. |
| **Max sample gap (s)** | 0.5 s | 0.1–5 s | Maximum gap between those samples. |
| **Min stable time (s)** | 0.3 s | 0–2 s | Minimum time the load must stay stable. |

## Example

The 1–500 g range limits the **Minimum cup weight** setting. There is no
independent maximum cup-mass setting. Fixed automation bounds apply to net
scale readings; they are not a configurable physical cup-weight range. Raw
reference acquisition uses the parsed sensing range: an empty reading of −522 g
after taring and removing a 522 g cup remains usable to measure the next cup.
This does not widen the net-weight bounds for placement automation or shot control.
Those bounds are −500 to +1000 g. If a previously tared heavy cup leaves a
lighter replacement below −500 g, remove it, tare the empty pan from
the scale's button, reconnect it, wait for stable zero, then place it again.

You set a cup down during the retare window. After three samples within 2 g
of each other, lasting at least 0.3 s, the firmware treats it as placed and
can retare. After tare, lifting the cup can produce a negative reading; a confirmed
reading at or below −3 g counts as removed. The removal-stop option is
configured separately in [Cup protection](../features/cup-protection.md).

If a light cup is below the configured minimum, placement will not qualify.
Choose a threshold below its stable empty weight, within the allowed range.
A very small threshold, even 1–2 g, does not stop the controller from
spotting the first drops: when coffee keeps flowing after a sudden step, the
first drops are still recognized.
Do not change stability parameters to compensate for an unstable surface.
Stable time must also fit inside the retare window and cannot exceed placement
samples × maximum sample gap. The server rejects inconsistent combinations.

For example, with a 2 g tolerance, 80→82→84 g is not one stable placement:
the complete window spans 4 g. Each new window must meet the same sample and
duration requirements. A lighter replacement can read below zero; its occupied
reference is retained so an unchanged negative plateau is not a second lift.
After a confirmed tare, the configured negative drop remains the in-shot
removal criterion; outside a shot, confirmed near-empty readings also detect
removal of light cups whose full mass is below that drop.

Related: [Cup protection](../features/cup-protection.md),
[Tare and retare](../features/tare-retare.md),
[Tare](tare.md).
