# Activation history

The activation history is a simple, independent diary of machine activations
and paddle attempts: shots, rinses, short runs, recognized machine wake
gestures, and attempts stopped by No-scale BBW protection. It shows what
happened and what you tried, without the measurement detail of the
[shot history](shot-history.md).

## What is recorded

Every activation the machine confirmed is recorded when its circuit opens,
including manual brews and rinses that the shot history skips. A paddle attempt
stopped by No-scale BBW protection is recorded when you release the paddle.
Each entry keeps the UTC instant and local time calculated with the zone's
offset when the activation ends, as well as its duration and type:

- **Shot** — an activation that outlasted the brew-by-weight protection
  window (12 seconds by default). The label records the intention to brew,
  even when no measured shot is saved in [Stats](shot-history.md).
- **Rinse** — a quick rinse cycle.
- **Backflush** — an automatic cleaning activation confirmed through the
  [Micra integration](../settings/linea-micra.md#automatic-backflush), shown with
  a cleaning-drop icon. Duration measures how long its activation circuit was
  closed. An interrupted confirmed backflush keeps this label; it does not
  certify that detergent cleaning finished, and it never counts in Stats.
- **Other** — a confirmed activation that ended within the protection
  window or exactly at its end, such as a brief paddle blip.
  A backflush attempt that opened the circuit before confirmation also records
  Other, regardless of duration. Waiting requests and refused starts add no row.
- **Power ON** — a paddle gesture recognized as a Linea Micra standby wake.
  It records how long the paddle kept the wake circuit active but never counts
  as a shot in Stats.
- **Blocked: no scale** — a paddle attempt that No-scale BBW protection
  prevented from starting. It records how long the attempt lasted, including
  repeated attempts in Require a scale mode, and never counts as a shot.

Other abandoned starts, where the machine never confirmed the activation,
record nothing. When the controller's clock has never synced, cards show
"no time" until it gets the time from the network.

A recognized Linea Micra standby wake creates one **Power ON** entry when the
paddle returns to OFF. See
[Linea Micra](../settings/linea-micra.md#recognize-paddle-wake-gestures).

The log holds up to **1000** activations. When it fills, the oldest entry is
dropped to make room for the newest. Entries enter memory when a cycle ends.
Flash saving follows when the controller is idle, so recording never slows down
a brew.

## In the Web UI

Open the **History** page (next to Stats) to browse the diary. The entries and
the sort and Clear controls normally appear at once with their data; a brief
loading animation covers them only while the device has not sent the initial
data yet — for example right after the interface starts or after a dropped
connection. From then on the diary keeps refreshing in the background while
you are on other pages, so History always opens instantly. The list loads
**20 entries at a time** as you scroll. New or deleted entries refresh the list
automatically from the first page; scroll to load more again. Sort by date,
newest or oldest first, from the sort control at the top. Entries are grouped
under a small heading for each day — "Today", "Yesterday", the weekday name for
the rest of the week, "2 weeks ago" for older weeks, and a short date like
"Sep 15" for older entries. Each compact card shows the activation type with
its time underneath, such as "Today at 08:47", and the duration on the right.
Hovering the time shows the exact date and time. Shots carry a coffee-cup
icon on the left of the card; the cup is only outlined when the scale had no
valid weight at the end of the shot. Rinses carry a droplet, and other
activations carry a lightning bolt, Power ON entries use the power symbol,
and Blocked: no scale entries show an X. When the clock
was not synced when the entry was recorded, the card shows "no time"
instead of a date, and those entries share a "no time" heading.

Narrow the diary to what you care about with the Filter control next to the
sort order. It opens a checklist of activation types — Shot, Rinse, Backflush,
Power ON, Other, and Blocked: no scale — and you can tick as many as you
like. With nothing ticked the diary shows everything; ticking one or more
types shows only those, and the list updates as you tick. While a filter is
active the Filter button shows a count badge and stays highlighted, so the
state is visible at a glance, and Reset at the top of the checklist clears it
in one tap. The checklist closes when you tap outside it or press Escape, and
it works the same on a phone and on a computer. The controller does the
filtering, so the diary still loads 20 entries at a time as you scroll and
every page is full of the types you picked, even when they are spread through
a long history; when a filter matches nothing at all, the page says so
instead of showing an empty diary. Export always downloads every entry,
whatever the filter.

Changing the saved time zone later does not rewrite earlier entry times.

Take the diary into a spreadsheet with the Export button. It downloads an
`activation-history.csv` file with every entry — its date and time, the UTC
timestamp, the duration, and the type — in the current sort order, newest or
oldest first, exactly as the page is set.

Delete a single entry with the ✕ on its card, or clear the whole diary with
the Clear button. Clearing asks for an explicit confirmation and cannot be
undone.

### With the machine off

Opened over HTTPS, the Web UI keeps a copy of the whole history on that phone
or computer, so **History** still works while the controller is out of reach.
See [Open it over HTTPS with a reverse proxy](../settings/web-access.md#open-it-over-https-with-a-reverse-proxy).

## Stats vs History

The two pages record different things and never substitute for each other:

| | Stats (shot history) | History (activation diary) |
| --- | --- | --- |
| Records | Brews with a valid settled yield over 2 g | Confirmed activations and paddle attempts blocked by No-scale BBW protection |
| Minimum duration | Longer than 12 seconds | Longer than the configured earliest weight stop time to carry a Shot label |
| Detail | Goal, yield, error, flow, guards, rating, curve | Time, duration, type |
| Capacity | 500 shots | 1000 activations |

A measured shot appears in both. A longer activation without a valid yield
can appear as Shot here while staying out of Stats. Deleting or clearing in
one page never touches the other.

USB: see [USB serial CLI](../SERIAL_CLI.md) for factory reset, which clears
both logs.

Related: [Shot history](shot-history.md), [Brew by weight](brew-by-weight.md),
[Wi-Fi](../settings/wifi.md) (timezone for time labels).
