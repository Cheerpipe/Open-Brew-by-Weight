# Quick rinse

A firmware **rinse** keeps the group on for a configured duration, then turns
it off. It is not a shot: no shot history, no last-shot overwrite, and it does
not count toward the scale-loss backup's recent shots. Rinses do land in the [activation history](../features/activation-history.md)
as `rinse` entries.

On Micra, a gesture that starts as a shot with a connected scale pauses cloud
API work; live updates keep arriving. As soon as it is recognized as a rinse,
that pause ends and API work can continue while the rinse is still running. Ordinary
network and scale-connection restrictions on API work still apply. Quick rinse
is separate from [automatic backflush](linea-micra.md#automatic-backflush) and
never receives its longer safety limit.

The rinse duration counts from the moment the group starts running. When a
running shot is demoted to a rinse (a paddle flip OFF inside the gesture
window, or a momentary press hold reaching the threshold), the time already
elapsed counts toward it: the group runs for the configured duration in total
from the original start, and the reported duration matches. A rinse that
starts from idle (Released hold, Armed, web) runs for the full duration
from its start.

**Settings → Shared settings → Quick rinse** is shown on paddle, momentary,
and reed firmware. Home **Start rinse** follows the same Quick rinse setting.

## When it applies

**Quick rinse** must be on. Its fresh-install and factory-reset default comes
from the machine profile: it is **on** for Linea Micra and **off** for the
Rancilio profiles. With it off, paddle short ON→OFF is a shot, and a momentary
long-press is native 1:1 (no `RINSE` cycle). Existing persisted choices survive
ordinary boot.

A Linea Micra paddle gesture recognized from a monitored OFF machine state —
current or stale, the last confirmed reading — only
wakes the machine. It never becomes a rinse, regardless of its hold duration.
See [Linea Micra](linea-micra.md#recognize-paddle-wake-gestures).

- **Paddle:** from Ready or during a brew, ON then OFF within the **gesture**
  time is a rinse. Holding ON past that time keeps a brew (or, if
  [No-scale BBW](no-scale-bbw.md) is Armed, may refuse to close the machine
  circuit). An Armed rinse from Ready does not run water unless **Allow rinse
  while Armed** is on; the gesture still uses up the missing-scale warning.
- **Momentary / reed:** a long-press that **starts from idle** (off / confirmed
  off) and reaches the gesture time is a rinse. In **Pressed** the shot
  starts on press and demotes to rinse at the threshold. In **Released**
  rinse starts directly (never a shot). A long-press during assumed/confirmed
  on of an existing shot is not a rinse. The same Armed-rinse rule applies:
  default off keeps the group from running and uses up the warning; the option
  on lets the rinse run.

A web rinse (when remote machine control is compiled in) uses the same duration
and requires Admin unlock. Without unlock, Home shows the version footer
instead of the Actions panel. Web rinse is refused when Quick rinse is off.

## Parameters

Machine-level, **Settings → Shared settings → Quick rinse**.

| Setting | Default | Range | Effect |
| --- | --- | --- | --- |
| **Quick rinse** | Machine profile | ON / OFF | Firmware rinse on/off. Initial profiles use On for Linea Micra and Off for Rancilio. |
| **Rinse gesture (s)** | 1 s | 0.1–5 s | Paddle: how long you can leave the paddle ON and still get a rinse when you flip it OFF. Momentary: how long to hold the switch from idle before a rinse starts. |
| **Rinse duration (s)** | 4 s | 0.5–10 s | How long water runs through the group after a rinse starts. |

Rinses are excluded from shot history by cycle type, not just by duration.

## Example

Default 1 s gesture / 4 s duration, with **Quick rinse** on. On paddle, flip
ON and back OFF within a second: the group runs for four seconds in total
(about three more after you flip OFF), then opens. On momentary, hold the
switch for at least a second from idle in Pressed mode: the shot demotes and
the group runs for four seconds in total from the press. In Released mode
the rinse starts from idle and runs for the full four seconds, then pulses
stop.

If a long button hold starts during an existing shot, it is not the idle rinse
gesture. If **Require a scale** blocks the attempt, rinse is also blocked unless
the physical temporary override was deliberately completed. If **Warn once, then
allow** is Armed and **Rinse without a scale** is off, the rinse gesture uses
up the warning and does not run water.

Related: [Paddle](paddle.md), [Momentary](momentary.md), [No-scale BBW](no-scale-bbw.md),
[Shot history](../features/shot-history.md).
