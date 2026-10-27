# Shot history

Every shot pulled with a scale is stored on the controller and shown in the
Web UI shot log. Use it to see how long a shot ran, why it ended, and how
close it landed to the target when brew by weight was in use.

When you open **Shots**, the Stats section, including its chart, and the
Shot history section, including its sort, export, and clear controls, normally
appear at once with their data. A brief loading animation covers them only
while the device has not sent the shot data yet — for example right after the
interface starts or after a dropped connection. From then on the data keeps
refreshing in the background even while you are elsewhere in the interface, so
the page always opens instantly with the latest shots.

Choose how far back Stats looks with **Last 10**, **50**, or **100** under the
averages. The averages and both charts follow your choice, and the note beside
it says how many shots they actually cover — fewer when you have not pulled
that many yet. Every time you open the Web UI it starts again at 10.

The Stats chart shows how those shots, the same ones behind the averages, land
against the shot time window. Each
part of the chart shows its share of your shots and how many there were: Fast
before the window, On recipe inside it and Slow after it. Below those numbers,
a smooth curve rises where your shot times cluster: a tall, narrow peak means
consistent shots, while a low, wide curve means they vary from shot to shot.
Each shot keeps the verdict it got when it was pulled, so editing a preset's
window later never changes past shots.

The chart starts on **Preset: All**; choose **Current** to see only the
active preset's shots. When every shot shown used the same window, the time
axis is in seconds and marks the window's edges, for example 27 s and 36 s.
When the shots come from presets with different windows, each shot is placed
against its own window instead: the axis shows **Window start** and
**Window end**, and how many seconds before or after the window a shot reached
its target. The chart appears once a shot has a measured arrival time.

Below it, the consistency chart answers a different question: not whether your
shots landed on recipe, but how alike they are. It measures time, not weight:
every shot already stops at its target weight, so what changes from shot to
shot is how long it took to get there. A barista whose shots all run a little
slow can still be very consistent. The chart looks at the same shots as the
averages above, keeps those with an arrival time, and follows the same
**All** or **Current** preset choice.

Each shot is a column, with the newest on the right; as new shots arrive, the
older ones move left. The chart always has room for at least 10 columns; with
**Last 50** or **Last 100** and more than 10 shots, the columns share the full
width, so 20 shots look the same under both choices and only grow thinner when
you have more shots to show. Each column starts at your average time to the target,
the dashed line, and shows how much slower (up) or faster (down) that shot was.
The shaded band covers 5 % of the average either side, about 1.4 seconds on a
28-second shot; columns that go past it are drawn in a stronger color. A shot
far outside the chart stops at its edge with a dashed cap; drag to it to read
its real time.

The limits scale with your recipe, because one second matters more on a
20-second ristretto than on a 45-second lungo. When the shots shown come from a
single preset, the chart and its numbers read in seconds. When they come from
several presets, each shot is compared with its own preset's average and the
chart reads in percent, so different recipes can share it fairly.

The numbers above the chart sum it up. The first is how much your time
typically changes from one shot to the next, with a verdict: **Very
consistent** up to 5 % of your average, **Fairly consistent** up to 10 %, and
**Varies a lot** beyond that. A single odd shot does not change the verdict on
its own. **Spread** is the gap between your fastest and slowest shot, and
**Shots** is how many are shown. With fewer than 5 shots the verdict reads
**Not enough shots yet**. Drag across the chart, or focus it and use the arrow
keys, to read each shot's time and how far it was from your average.

Stopwatch and gram-weight icons sit beside the large duration and yield values
in each shot card and beside Avg time and Avg yield in the Stats summary.
Each icon and value are centered together within their column. The icons
follow your chosen light or dark theme. History cards use equal left and right
spacing for their measurements and charts, with the shot's actions menu (⋯)
in the top right corner.

## What is recorded

Every confirmed, non-rinse cycle that started with a scale connected is
stored, however short it was and however it ended. A shot that lasts **more
than 12 seconds** with a valid settled final yield of **more than 2 g** is a
**good shot**; any other is a **bad shot**: too short, without a yield, or
with a yield of 2 g or less. Manual stops, timer-only brews, weight stops on
any part of the cut line, and time or safety limits all use this same rule.
The 12-second rule is fixed; changing BBW's start-of-shot protection time
changes weight stopping, not which shots are good.

Bad shots are kept so you can look back at what went wrong, but they never
count. Only good shots feed the Stats averages and charts, Home's last shot,
the preset statistics (**Last 10 shots**, first-drop timing and the grind
hint), drip learning and its quick start, the scale-loss backup's recent
shots, the Home Assistant statistics and last shot, and the CSV and JSON
exports. A bad shot does not teach drip learning anything, even right after
it is pulled. The shot list hides bad shots unless you ask for them with
**Show bad shots**; see [In the Web UI](#in-the-web-ui).

Typical fields include local time (from the saved time zone at shot end),
duration, the exact preset name captured at shot start, goal and yield (the
shot's actual output), error, average flow, first-drop time, the scale used
(your chosen name, otherwise the suggested model name, or the advertised
Bluetooth name when neither is available), the late-tare time when the cup was
tared mid-shot, how the shot ended (**Ended** shows the Fast, On recipe or
Slow verdict for weight stops), the cut settings it ran with, `shot_type`, `cut_type`
(`auto`, `manual`, `limit`), `stop_detail` (for example
`normal_target`, `activator`, `web_stop`, `wall_limit`, `hard_limit`,
`extended_max_weight`, `cup_removed`), and a manual `rating` from 0
(unrated) to 5. Rate a stored shot from its history card; tapping the current
star again clears the score. A friendly end time, such as “Today at 19:06”,
appears on the Stats history cards; it is shown only when the clock was set
when the shot finished. The preset snapshot is shown on the Home shot hero
card and on every Stats history card. Renaming or deleting a preset later does
not rewrite a shot's displayed name.

**Scale** appears after **Preset** and keeps the name captured for that shot,
even if the scale disconnects or is renamed later. It uses the full width of
the card, so long scale names have room before they wrap. Existing shots
keep their original names. A Bluetooth name longer
than the friendly-name limit is shortened to fit. **Late cup**, after
**1st drop**, shows the successful late tare's elapsed shot time, or **None**
when no late tare succeeded. Its chart label always uses one decimal, such
as **3.0 s**. The initial shot-start tare is not a late tare.

**Target temp**, after **Max flow**, shows the coffee temperature the
machine was set to when the shot ended, such as **93.5°C**. It only appears
when the machine integration reports a target temperature, so shots pulled
without that connection simply leave the space to the fields after it.
**Dose** and **Ratio** follow it: the dose the active preset had when the
shot started, such as **18.0 g**, and the brew ratio that came out of the
shot, the recorded yield over that dose, such as **1:2.0**. Both are kept
with the shot, so editing the preset later never rewrites them.

**Drip**, after the error, shows how much coffee kept dripping into the cup
after the machine stopped: the settled yield minus the weight at the stop, such
as **+1.6 g**. It shows **—** when the scale gave no settled reading after the
drip delay, or when the shot's chart has no reading from the moment it stopped.

**Cut at**, after **Scale**, shows the weight on the scale when the machine
stopped and the shot time of that stop, such as **34.6g · 27.4s**. **Drip
delay** shows how long the scale waited for the last drips before it saved
that shot's final weight, so changing the setting later does not rewrite older
shots. Shots saved before this was recorded show **—**.

If a late retare succeeds and a fresh reading confirms its new zero, an earlier
first-drop time and marker are discarded. Later coffee can supply a new time
and marker; without that replacement, first-drop time and **Avg flow** are
unavailable. The rest of the shot curve is retained. **Late cup** records
successful command completion, so it can also appear when zero was not
confirmed and the earlier first-drop record was kept.

The log holds up to **500** shots, good and bad alike. When it is full, the
newest shot replaces the oldest one, so a run of bad shots can push out older
good shots. The following are never stored:

- Rinses and starts that were not confirmed
- Brews started without a scale, unless a scale connected during the brew and
  gave it a good yield

Home shows the current activation as it happens. After the machine stops, its
duration stays fixed while accepted drip readings continue updating the curve.
The shot card shows the yield with its target, such as **36.2 g / 36 g**, and
the shot time, such as **30.4 s**. Between them, slightly smaller, is the flow:
while the shot runs, how fast coffee has been landing in the cup over the last
second, such as **1.6 g/s**, so it moves steadily instead of flickering; once
it ends, the shot's average flow from the first drop. The
flow appears shortly after the first drop, only for shots made with a scale,
and steps aside while a hand rests on the scale.
Below them, a chart draws the shot's weight over time on top of the preset's
cut line (dashed) and its shot time window (the shaded band), so you can see
at a glance where the shot landed against the recipe. A finished shot is drawn
against the preset as it was when you pulled it. First drop, average flow and
the difference from the target are on the shot's card in Stats.

The top line of the card names the result: **On recipe**, **Fast**, **Slow**
or **Max time**. When the cut line did not stop the shot at the target, a pill
on the right says why: **Extended**, **Stopped early**, **Channeling** or
**No scale** (see [Fast and slow shots](fast-and-slow-shots.md)). When the
scale dropped and the [scale-loss backup](auto-to-manual.md) finished the shot,
the yield is shown as an estimate, such as **~36 g est.**, when one is known.

The card's color follows the result: green on recipe, red when fast and blue
when slow. While a shot runs with the default **By forecast** colors, the card
fades from coffee brown toward the color the controller expects — growing
stronger as the shot progresses and its confidence grows — and a slow shot
shows the weight it is heading for (**Slow — about 28 g at 36 s**), while a
stalled one says what it is tracking to (**Stalled — tracking to ~22 g**).
You can choose clock-based colors, or keep the usual brand color, in
[Admin → Frontend → Shot card colors](../settings/theme.md#shot-card-colors).
The timer comes from the controller and pauses on screen if the connection is
lost; **Reconnecting…** indicates that the displayed result may be stale.

If an activation is not a good shot, the page that observed it keeps its
result for 30 seconds after analysis finishes, then returns to the latest good
shot. A temporary reconnection preserves the remaining interval; reloading the
page opens the latest good shot instead. Starting another activation
immediately replaces the temporary result. Confirmed activations also appear
in the separate [activation history](activation-history.md). Drip learning and
the scale-loss backup's recent shots read good shots only, and add their own
rules on top.

The controller collects accepted scale readings in memory while the shot runs
and saves the completed curve after the configured drip delay. Each reading
keeps its actual arrival time, including repeated weights and changing scale
cadence. Duration ends when the machine circuit opens. During the drip delay,
accepted readings continue as a faint dashed line without fill. A small hollow
circle marks the last reading when it matches the settled **Yield**.
The drip readings keep their actual times and never change the earlier curve
or its flow. Older shots without recorded drip readings keep their original
curve; no drip tail is reconstructed from the final weight alone.
Isolated readings and event weights appear as small dots at the curve's line
thickness, including when the chart is stretched across a wide screen.

When you steady the cup or move a chilling ball during a shot, the scale
briefly reads your hand as well as the coffee. With **Avoid accidental touch**
on, the controller recognizes those readings and does not count them as coffee.
The chart still shows them as faint grey dots, labelled **Touch ignored**, so
you can see when it happened, while the weight line continues straight
through as if your hand had not been there. A reading or two at the very start
of a touch can still nudge the line. A load that stays on the scale is
eventually counted as weight, and the line starts a new segment there.

A curve can hold 1201 readings within 60 seconds of shot start, including
the drip tail. If the readings exceed either limit, the captured prefix
remains available and the chart says **Incomplete curve**. Brew stopping and
the shot's summary still work. Known rejected readings, a new tare reference
and scale loss leave breaks in the curve. A healthy slow scale does not
create a break merely because its readings are farther apart. An incomplete
tail has no final-yield circle. Removing or replacing the cup ends drip capture.

The first-drop marker and late-tare cup marker keep their event times. Each
curve takes its shot's Fast, On recipe or Slow colour, and a scale-loss stretch
its own colour, without adding competing time labels;
the time axis uses 10-second references. Home shows the live memory curve
during a shot and the newest good shot's exact curve while idle.

Observed removal or a new placement during the drip delay preserves the weight
captured at shot end and discards post-drip learning, regardless of the idle
automatic-tare setting. A single fresh reading consistent with the known empty
pan is enough to discard this optional analysis, but cannot trigger a replacement
tare. If a physical swap is completely absent from the scale notifications,
weight alone cannot establish that the final reading belongs to another cup.

## In the Web UI

Open the shot history table to browse rows, export or delete one shot from
its ⋯ menu, clear the whole log, or export every good shot as CSV or JSON.
Clearing requires an explicit confirm.

Home and Stats read the same shot history. Deleting the newest good shot
shows the next newest good shot on Home; clearing the log empties the idle
card.
Factory reset clears both histories. The [activation history](activation-history.md)
records every confirmed activation, including rinses and cycles that do not
qualify as shots. Deleting or clearing shot history never changes that diary.
New rows appear after the configured drip delay. If flash saving is still
pending, the API reports `savePending`; a failed write remains pending for retry.
Once saving completes, the newest shot and its curve survive a restart.
Power loss before saving completes can still lose pending changes.

When you first open **Shots**, both **Stats** and **Shot history** show a loading
wave. They fade away together once the summaries and history are ready. The
wave over Stats does not add space to the panel or resize it when it disappears.
With Wi-Fi sleep enabled, a large history may take a little longer to appear.
You can leave the page while it loads and return later.

Sort the list by **Date** or **Rating**, ascending or descending. Date
defaults to newest first. Rating puts unrated shots (0 stars) at the end
in both directions; equal scores keep newer shots first. The table loads
**10 shots at a time** as you scroll.

Narrow the list with the **Filter** control next to the sort order. Tick
**Fast**, **Slow**, or **On recipe** to see only shots with that result; only
shots stopped by weight with a measured arrival time have one, so a result
filter leaves the others out. When your shots used more than one scale or more
than one preset, the checklist also lists them, so you can look at one
grinder setting or one coffee on its own. Ticks within a group add to each
other, and ticks in different groups must all match: Slow plus your house
preset shows only the house preset's slow shots. The filter works with either
sort order, every page you scroll to is full of matching shots, and the Filter
button shows a count badge while a filter is active; Reset clears it, and
tapping outside or pressing Escape closes the checklist. When nothing matches,
the list says so. The filter changes only the list: the averages, charts, and
**Export** still cover your good shots as described below.

The list shows good shots only. Tick **Show bad shots**, at the bottom of the
checklist, to add the bad ones; it counts in the Filter badge like any other
tick, Reset clears it, and it is off again whenever you reopen the Web UI. It
works with the other ticks and either sort order: Show bad shots plus your
house preset lists that preset's good and bad shots. Each bad shot carries a
grey **Bad** label beside its time with the reason: **short** (12 seconds or
less), **no yield**, or **low yield** (2 g or less). A bad shot shows only what
the controller stored, so a missing yield, flow or ratio reads **—** instead of
being estimated from its curve. The preset and scale choices in the checklist
come from your good shots.

Changes arrive automatically while the
page is open. After an entry is added, rated, or deleted, the list refreshes
from the first page in your chosen order; scroll to load more again. **Export**
downloads every good shot newest-first, whatever the on-screen sort or filter. A large
export can take up to 90 seconds on a slow connection. Wait for the download
before changing the sort or leaving Stats.

The Time column reads the way people talk: "Today" or "Yesterday" with the
clock time to the minute, the weekday name for the rest of the week, then
"2 weeks ago" for older weeks, and a short date like "Sep 15" (with the year
when it is not the current one). Hovering a time shows its exact date and
time to the second, and the CSV export always keeps that full detail no
matter how the table displays it.

Stats uses up to the newest **10, 50, or 100 good shots**, as you choose,
for duration, yield, available flow, daily count, and both Stats charts,
regardless of how the table is sorted, filtered, or how far you have scrolled
it. **Avg BBW error** independently uses up to the same number of newest
**good shots stopped by weight** in the full history, skipping manual shots and time
or safety stops. Each miss is measured against the planned stop — the cut line
when the stop was commanded, which is above the target for a fast shot and
below it for a slow one — and the absolute percentages are averaged, so
overshoots and undershoots cannot cancel each other. It is unavailable when no
shot has been stopped by weight yet. For example, a fast shot whose cut line
stood at 38.0 g and that yielded 38.4 g counts as a 1.1 % miss. **Daily shots**
averages the dated shots over the calendar days they span, including both ends.
An erase-all firmware installation starts with empty shot history.
A shot kept from an older firmware that does not meet the good-shot rule, for
example one recorded at exactly 12.0 s or with a yield of 2 g or less, now
counts as a bad shot.

In its Stats history card, each shot shows everything at once. At the top are
its duration and yield in large numbers, then when you pulled it, with a
colored badge saying whether it landed **Fast**, **On recipe** or **Slow**.
Below come its target, error, flow, late cup, first drop, how it ended, shot
type, preset, scale and your star rating, which you can change right there.
Then comes its **preset chart**: the
preset's cut line, its on-recipe corridor, and the shot's own
weight curve drawn over it. The chart always uses the preset as it was when that
shot was brewed, so editing or deleting the preset later never changes it. Only
the zone the shot ended in is colored: fast, slow, or on recipe. Timer-only
shots show every cut line as off and no colored zone.

Tap or drag anywhere on the chart, or use the arrow keys when it has focus, to
move a cursor along the shot's curve. The line under the chart reads the time,
the weight at that moment, and the cut line at that time. While your finger or
mouse is on the chart, a small bubble beside the cursor shows the same time and
weight. The cursor starts at the last reading, the **Final weight, after
drips**. When drips kept falling after the machine stopped, a smaller dot
labelled **Shot stopped** marks the moment the shot was actually cut, with its
real time and weight. It usually sits a little before or below the cut line,
because the shot is stopped early enough for the last drips to land on target.

Under the preset chart, a single weight and flow chart shows how the shot
actually poured: the weight curve and the flow rate together, on one time line
that includes any recorded drip tail. Like the preset chart, its legend sits
below it and names each line, including the dashed **Drip after the stop**. Read weight on the left
scale and flow on the right one; both share the same reference lines, one
every 10 g, with the flow scale chosen so its values fall on those same lines.
The weight curve uses the same colors as the preset chart, turning red or
blue from the moment a fast or slow guard stepped in, and flow is always grey.
Both fill with a soft shade that fades toward the bottom. The chart is as
tall as the preset chart above it, so grams look the same size in both. Shots
without a preset chart show only this chart below their details.

This chart has a cursor too, and it works the same way: tap or drag anywhere
on it, or use the arrow keys. Two dots move together, one on the weight curve
and one on the flow curve, so you can read both at the same moment. While your
finger or mouse is on the chart, a bubble shows the time, the weight and the
flow rate, each value marked with its line's color. Where there is no flow
reading, such as before the first drops, the flow dot hides and the bubble
shows a dash instead. The cursor starts at the shot's stop point.

Every filled chart in the Web UI uses that same fading shade: this chart, the
preset charts in Stats, Home and Settings, the Stats arrival-time density, and
the curve and the On recipe corridor on the Home shot card.
On a narrow screen, a few numbers beside these charts may be hidden when they
would overlap. The lines and measurements stay in place, and more numbers
appear again when there is room.

The Flow rate chart measures the weight change over the preceding second using
the readings' actual arrival times. If a second starts between two readings,
their weights supply an interpolated starting value. The whole window needs
continuous readings on or after first drop. Before enough readings arrive,
or after a known rejection, tare or scale-loss break, measured flow is unavailable.
The flat line before first drop is only a visual reference. With uninterrupted
readings, a short visual connection joins it to the first available flow point.
That connection is not a measured rate and does not contribute to Max flow or
the CSV export. Known interruptions remain gaps. Flow is not measured across an
ignored touch or the half second before it, so a hand on the cup never shows up
as a burst of flow; the flow line has a short gap there instead.

Each estimate is drawn at the middle of its measurement window, usually half a
second before the latest reading. With a healthy scale reporting less often
than once per second, flow describes the actual longer span between readings
and is drawn at its midpoint. A long interval alone cannot distinguish a slow
scale from an interruption that the controller could not detect. Repeated weights
are retained. When several readings share an arrival time, the last one supplies
the endpoint; earlier readings at that time have no separate flow value.
Event annotations and settled yield never enter the measured flow series.
Flow calculations preserve first-drop and cutoff timing to the millisecond,
so readings just before the cutoff remain available and a window cannot start
before first drop.

The cards call shot output **Yield** while chart, goal, scale, and cup labels
continue to use Weight where they describe weight itself. **Avg flow** remains
the final yield divided by the time after first drop. **Max flow** is the highest
non-negative estimate from supported windows ending before or at machine cutoff; Home
shows the peak so far during a live shot, and saved cards reproduce it from
the stored observations. Falling weight contributes 0 g/s. Max flow is
unavailable when the curve has no supported window. For an **Incomplete curve**,
Max flow is marked **(recorded)** and describes only the captured portion of the
shot. Recorded post-stop readings also support flow estimates using the same
calculation. Their chart continuation is faint, dashed and unfilled from the
machine's cutoff time onward. Drip flow does not change **Max flow** or shot
duration. Each estimate stays at its window midpoint, so the flow line can end
before the last weight reading; it is not extended with an assumed zero.
No extra smoothing changes the plotted values.

The controller stores only the weight curve. The Flow rate chart and Max flow
are derived locally, so viewing or reloading them does not create another
history record or flash write.

### With the machine off

A phone or computer that opens the Web UI over HTTPS keeps a copy of every
good shot, so **Shots** still works while the controller is out of reach. The
copy leaves bad shots out, so **Show bad shots** needs the controller.
See [Open it over HTTPS with a reverse proxy](../settings/web-access.md#open-it-over-https-with-a-reverse-proxy).

## Export and share shots

**Export**, next to Filter, opens a small menu with two formats. Both always
include every good shot, and only good shots, whatever the list's filter or
sort:

- **CSV** is a table for Excel, Numbers, or Google Sheets, one row per shot.
- **JSON** keeps everything the controller recorded about each shot, including
  its full weight curve, in a structured file. Use it as a backup, to analyze
  your shots in your own tools, or to bring them into other apps. Its layout is
  published as a
  [JSON Schema](../schemas/openbrewbyweight-shot-export-v2.schema.json), with an
  [example file](../schemas/openbrewbyweight-shot-export-v2.example.json), so
  other projects can read it reliably.

Each shot card has a **⋯** button in its top right corner. Its menu acts on
that shot only:

- **Open in Beanconqueror** hands the shot to the Beanconqueror app on the same
  phone or tablet, ready to save as a brew with its dose, yield, time, first
  drip, target temperature, rating, and weight curve. Open the Web UI on the
  device where Beanconqueror is installed. It needs a Beanconqueror version
  that accepts brews from other apps; versions up to 8.7.0 do not, and on a
  device without a suitable app nothing happens.
- **JSON** downloads the shot in the same format as the full export, so one
  shot and the whole log read the same way. A bad shot's file says why it is
  bad.
- **Gaggiuino** downloads the shot in the format Gaggiuino machines use, which
  [Visualizer](https://visualizer.coffee) can import. It carries the weight,
  flow, and target temperature over time. Open Brew by Weight measures weight
  only, so pressure, pump flow, and water temperature stay empty there.
- **Visualizer** downloads the shot as a file made for
  [Visualizer](https://visualizer.coffee): on its upload page, drop the file in
  or pick it. Besides the weight and flow curves, it brings the preset name,
  dose, target and final weight, target temperature, and your rating. Stage
  lines mark the first drop and the cut. The shot's notes in Visualizer list
  what has no field of its own there: how the shot ended, the cut weight and
  time, drip, the scale, the brew-by-weight algorithm, and how far the yield
  landed from the target. Grinder, beans, and roast stay empty for you to fill
  in on Visualizer. A shot recorded before the controller knew the time is
  dated when you download it.
- **Delete shot**, at the bottom of the menu, removes the shot after you
  confirm, as before.

Exporting never changes your shots, so it also works while settings are locked.

## Read a result

The CSV exports `local_time` as the local date and time saved for that shot,
`ended_at_unix` as UTC Unix seconds, and `tz_off` as the signed offset in
minutes applied when it ended. For example, two shots in `America/Santiago`
may have different `tz_off` values in winter and summer. Changing the saved
zone later does not recalculate either row. If the clock was unavailable,
`has_wall_time` is `0` and all three time cells are empty. A zone name is not
recorded per shot.

CSV retains the scalar shot columns, including `yield_g`,
`yield_source`, `max_flow_g_s`, the BBW fields and `preset_id`. Each shot
also exports `target_temp_c`, the machine's coffee target in °C at the shot
(`-1` when the integration reported none), plus `dose_g` and `ratio`, the
preset dose and the shot's brew ratio kept with the record.

The file opens cleanly in spreadsheet apps: it is marked as UTF-8, so preset
and scale names with accents read correctly, and a text cell that starts with
`=`, `+`, `-` or `@` (a preset named `-Ristretto`, say) begins with an
apostrophe so the spreadsheet keeps it as text instead of treating it as a
formula. Numbers, negative ones included, stay plain numbers.

Every shot also keeps the cut settings it actually ran with, so editing or
deleting a preset later never changes how a past shot reads:
`fast_before_s` and `slow_after_s` (the shot time window), `fast_side` and
`slow_side` (`1` when that side was on), `fast_margin_g` and `slow_margin_g`,
`delay_s` (earliest weight stop), `wall_s` (max shot time), and each side's
transition: `fast_ramp_s` and `slow_ramp_s` (its length), `fast_curve` and
`slow_curve` (`early`, `even` or `late`) and `fast_curve_hardness` and
`slow_curve_hardness` (`soft`, `medium` or `hard`).
`cut_line_g` is where the cut line stood when the stop was commanded, and
`cut_raised` / `cut_lowered` are `1` when it was above or below the target.
`arrival_s` is when the shot reached the target, or would have reached it,
and `verdict` is `fast`, `on_recipe` or `slow` for shots stopped by weight;
other endings leave it empty. `channeling` is `1` when a flow surge was
detected, and `scale_loss_source` names how the scale-loss backup chose its
stop time (`projection`, `history`, `window` or `fixed`).
Spreadsheets using the former `ext_guard`, `ext`, `slow_guard`, `slow_ext`,
`max_rec_g`, `min_bbw_brew_s` and `early_s` columns must switch to these
names.
After those columns, `curve_truncated` reports `1` for an incomplete
capture and `0` otherwise. `curve_break_before` lists the zero-based
indices of readings starting a new segment, separated by semicolons.
`curve_held` lists, the same way, the readings ignored as a touch: their
weight columns show what the scale read, and their flow columns stay empty.
Next come the stop details: `cut_s` is the shot time when the machine stopped,
`cut_weight_g` the weight on the scale at that moment, `drip_g` the settled
yield minus that weight (empty unless the yield was read after the drip
delay), and `drip_delay_s` the drip delay that shot used. All four stay empty
when the shot has no recorded stop; `drip_delay_s` is also empty for shots
saved before it was recorded.

Each reading has an ordinal group: `sample_1_time_s`,
`sample_1_weight_g`, `sample_1_flow_g_s`, then the same three columns
for sample 2 and so on. Times are the original elapsed arrival times in
seconds, weights are grams, and flow is grams per second. A flow cell stays
empty when that reading has no supported flow window. Recorded drip readings
include their supported flow estimates too. The time is the window's
ending observation time, while the chart uses its midpoint; arrival times retain
millisecond precision (up to three decimal places in seconds). The chart,
Max flow and CSV use the same estimates. Missing curves
and groups beyond a shot's captured length leave empty cells. The export
does not resample to a grid, so different scale cadences remain visible without
creating a column for every distinct arrival time.

The underlying curve JSON carries the readings as one compact stream (`wD`,
centigram weights with integer elapsed milliseconds), which the page expands
into the chart and the export. `wBreakBefore` supplies the same
segment-start indices, `wHeld` the ignored-touch indices, and `wTruncated`
reports completeness.
Spreadsheets using the former `yield_dt_s`, `yield_<time>s` or
`flow_<time>s` columns must switch to these ordinal sample groups.

`preset_id` (JSON `presetId`) and JSON `presetName` are captured from the preset
used for that shot, not the currently selected recipe. The name is a snapshot,
not a lookup through the mutable preset list, so it survives switching, renaming,
or deleting the preset. Pre-V3 records have unknown identity (JSON 0, CSV empty),
IDs are
local to the controller, use 1–255 and may be reused after allocation wraps or
settings are reset; use the captured name when comparing historical shots.

`offset_g` is the compensation captured at shot start, **before learning**,
in grams at 0.01 g storage resolution. Zero is valid. It is not the baseline,
next learned offset or measured post-shutdown water. `bbw_alpha` is the drip
learning speed captured at shot start, in exact hundredths from 0.01 through
1.00 (for example 0.37). It is not a gain selected after this shot's result.
Learning applied is `1`/`0` in CSV and true/false in JSON; a skipped shot still
retains its assigned gain. For example, appended CSV values can be
`corridor,1,0.37,1` and later `corridor,1,0.50,1` for the same preset.

The algorithm column is descriptive: it records the policy each shot ran
under, currently always `corridor` (the smooth cut). It is a read-only
identity, not a setting.

Algorithm identity describes the shot's configured policy even when a guard
or manual action ends it; use `stop`, `shot_type` and `cut_type` to interpret
the outcome. EWMA time/safety-limit outcomes remain in good history with
learning-applied false; only a good shot ended by a normal weight-target cut
can train EWMA, and a bad shot always shows learning-applied false.
Compare shots like for like using each shot's captured `bbw_alpha`,
`offset_g` and the firmware identity recorded alongside the dataset; recipe
changes and resets belong in those notes too. Export the CSV or JSON before a firmware
update so pre-update shots keep their original algorithm identity.

Average flow uses final weight (including accepted post-drip) minus baseline,
divided by duration after first drop. It is not terminal flow at cutoff.
The Flow rate chart is different: it shows the non-negative weight change over
supported measurement windows and can rise or fall throughout the shot.
Error and average flow share final weight algebraically, so their correlation
does not prove a residual-flow mechanism. The dashed weight tail shows captured
post-stop drips; the flow chart also shows their supported rates as a dashed
continuation. See
[BBW learning](brew-by-weight.md#cutoff-behavior-and-learning).

| Stop detail | Meaning |
| --- | --- |
| `normal_target` | Normal weight endpoint, including offset/prediction |
| `touch_weight_fallback` | Touch protection blocked the weight stop and the sustained-weight backup ended the shot; Ended shows Touch fallback on Home and Stats, matching Home Assistant's Last shot stop detail |
| `extended_max_weight` / `extended_min_time` | A fast shot stopped on a raised cut line (**EXTENDED**) |
| `slow_max_time` / `slow_min_weight` | A slow shot stopped on a lowered cut line (**STOPPED EARLY**) |
| `auto_to_manual` | The [scale-loss backup](auto-to-manual.md) finished the shot by time |
| `activator` / `web_stop` | Physical / authenticated Web stop |
| `wall_limit` / `hard_limit` | Operational / hard time limit |
| `cup_removed` | Cup-removal protection requested stop |

For example, 39 g against a 36 g goal with a Fast stop detail is not the same
calibration problem as a normal target cut followed by excess drip. Check the
reason before resetting learned offset. A live 2 s cycle can appear on Home,
but once completed it is excluded from shot history and does not replace the
idle last-shot card. An unrated shot uses rating 0.

USB: `CLEAR_SHOTS` (see [USB serial CLI](../SERIAL_CLI.md)).

Related: [Brew by weight](brew-by-weight.md), [FAQ](../FAQ.md),
[Wi-Fi](../settings/wifi.md) (timezone for wall-clock labels).
