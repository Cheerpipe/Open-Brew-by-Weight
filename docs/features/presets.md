# Presets

Presets store brew recipes. A new controller comes with five factory presets:
**Double · Adaptive**, **Single · Adaptive**, **Double · Fixed**,
**Single · Fixed** and **Manual** (active until you pick another one, for
example on the first-time setup page). Adaptive presets run fast shots a little
longer and stop slow shots a little sooner; Fixed presets always stop at the
target (see [Fast and slow shots](fast-and-slow-shots.md)). **Manual** never
stops the shot for you: pick it when you want to end each shot yourself, then
switch back to a brewing preset to stop by weight again.

Open **Settings → Presets** while the machine is idle. The preset actions sit
in a bar above the list (see [Preset actions](#preset-actions)). Each preset appears as a
card with its name, an **Adaptive**, **Fixed** or **Manual** badge, and a summary such as
**18 → 36 g · 1:2.0 · 27–36 s** (dose, target yield, ratio and shot time
window). Tap a card to use and review it. Below the cards, **How this preset
cuts** draws the preset's cut line. Besides your target, it names how far the
line moves for fast and slow shots, for example **Up to 42 g** over its high
level and **Down to 33 g** under its low one. The editor groups its settings:

| Group | Settings |
| --- | --- |
| **Recipe** | Brew by weight, Dose (g), Target yield (g), Brew temperature (°C, Linea Micra only), Shot time window (s), Grind hints |
| **Fast shots** | Extend fast shots, Extend by up to (g), Transition (s), Transition curve and its hardness |
| **Slow shots** | Shorten slow shots, Shorten by up to (g), Transition (s), Transition curve and its hardness |
| **Time limits** | Earliest weight stop (s), Max shot time (s), Backup stop time (s) |
| **Drip compensation** | Expected drip weight after the shot stops with its recent-shots chart, Learning speed, each applied with its **Set** key |

Each change is saved to the preset in use as soon as you make it; the line
below the recipe reads **Current preset:** followed by that preset. Everything
under **Shared settings** further down applies to every preset and is saved the
same way.

On Home, the **Presets** section lists the recipes as rows: the active one is
marked with a filled circle and expands to show how it cuts, for example
**Before 27 s · Fast · up to 42 g**, **27–36 s · On recipe · 36 g** and
**After 36 s · Slow · down to 33 g**. Tap the arrow under these lines to open
a small chart, and tap it again to fold the chart away. The chart shows
the same cut line over the shot, the colored fast, on-recipe and slow zones,
the opening seconds where weight never stops the shot, and dots where your
recent shots on that preset reached the target. Touch it or drag along it to
try shot timings, just like **How this preset cuts** in Settings: the example
shot follows your finger right away, and swiping up or down over either chart
still scrolls the page. A note below
it says where its reference pace starts.
The shot card at the top of Home draws a lighter version behind your shot: the
cut line with its **Up to** and **Down to** weights, the reference paces that
separate fast, on-recipe and slow, the on-recipe zone between them, dotted
marks where the time window starts and ends, and a faint hatch over the
seconds when weight never stops the shot.
Low in the chart, a soft shaded area traces your flow, the same flow the shot
history shows, smoothed so scale jitter and splashes don't show as spikes;
after the machine stops, the drip tail fades further.
Tapping another row applies that recipe. These rows show the recipe's own values; learned
drip compensation and time limits still apply.

The preset list and its action bar stay hidden behind a loading animation
until presets are ready. The other Settings groups remain available as
collapsed groups. Once loaded, the available actions depend on the selected
preset and whether editing is allowed.

## Preset actions

| Action | What it does |
| --- | --- |
| **Relearn** | Use after a new bag or a change to your dose, basket or grinder. Grind hints, the scale-loss backup and the usual first-drop time on the preset graph learn from your next shots; the shots so far are set aside. Your settings do not change. Ordinary grind tweaks don't need it: grind hints notice them on their own. |
| **New** | Creates an Untitled preset from the Double · Adaptive values. |
| **Duplicate** | Copies the selected preset into a new one. |
| **Restore** | Factory presets only: returns the preset to its factory values and starts its shot statistics over. Shared settings do not change. |
| **Delete** | Your own presets only: removes the preset. This cannot be undone. |

## Change the target weight from Home

The active preset's **On recipe** row in Home's **Presets** section has a
small pencil next to the target weight. Tap it to open a panel at the bottom
of the screen. It shows the weight, the dose and the ratio as two small
labels (for example **18.0 g in** and **1:2.0**), and a sliding scale between
**−** and **+** buttons:

- The scale moves with your finger: drag it left for a higher weight, right
  for a lower one. Slow drags follow your finger exactly, a tenth of a gram
  at a time; a quick flick moves up to three times farther. Let go of a
  flick and the scale keeps sliding a little before it settles; touch it,
  tap **−** / **+** or **Reset** to stop it at once.
- Tap **−** or **+** to change the weight by exactly a tenth of a gram. Hold
  either one and it repeats, then speeds up to a whole gram at a time.
- Every whole gram on the scale carries its number, with smaller marks at the
  half gram and the tenth. Marks the recipe can't use are dimmed, and the
  scale cannot stop on them.
- The scale only offers targets the recipe can actually use: from 10.0 g to
  200.0 g, in tenths of a gram. The extra and reduced yields for fast and
  slow shots are kept as a distance from the target, so they move with it.
  When you reach either end, a small **Target minimum** or **Target maximum**
  label appears next to the weight.
- **Reset** appears once you move away from the recipe's weight; tap it to go
  back.
- **Save** lights up once the weight differs from the recipe's. Closing the
  panel — with **Save**, a swipe down, or tapping outside — saves the new
  weight to the active recipe right away. The change is the same as
  saving the target in Settings.

Opening `http://<device-address>/?edit_weight=1` also opens this panel.
The address parameter is used once and removed, so refreshing the page
afterwards shows the normal Home.

## Create or change a recipe

1. Tap the preset you want to start from.
2. Select **Duplicate** to copy that recipe, or **New** for an Untitled
   preset with the Double · Adaptive values.
3. Rename the card and edit its settings; each change is saved as you go.
   Values that conflict with another field are flagged in yellow and saved
   once the conflict is fixed. Shot statistics only count shots made with the
   preset's current window and a target within 1 g, so if you change the
   window or move the target further, a note after the save says they start
   over. Going back to the earlier values brings those shots back. The usual
   first-drop time does not depend on either and stays.
4. Switch to another preset and back to check the saved recipe.

Duplicate names receive a suffix, such as "Espresso copy 2"; long names are
shortened so the suffix fits. You can keep up to eight presets in total, the
five factory presets included. Once you have eight, New
and Duplicate stop creating cards — delete one of your own presets to make room
for a new one. Factory presets can be restored but not deleted. The active
preset survives reboot.

## What is saved where

| Scope | Settings |
| --- | --- |
| Preset (**Settings → Presets**) | Brew by weight, dose, target yield, shot time window, grind hints, fast and slow shot options and amounts, earliest weight stop, max shot time, the optional scale-loss stop time, starting drip and learning speed, learned drip, learning speed and its provenance, and the preset's shot statistics; Linea Micra builds also retain a brew-boiler target |
| **Shared settings** | Linea Micra, paddle or switch behavior, quick rinse, scales, tare, cup detection, cup protection (including avoid accidental touch and stop after sustained weight), No-scale BBW, the scale-loss backup switch, alerts; Bluetooth and network settings are in Admin |
| Header panel / Home Assistant switches | Temporary: **By weight / Manual** and the six guards change only until you pick another preset or restart. Nothing is saved to the preset or to Settings, and picking a preset brings back its saved values. |

To make an intentional recipe change permanent, edit and save it in Settings;
the target weight alone can also be set from Home as described above.
**Stop after sustained weight** is a shared setting for every preset and is on
by default. Turning
**Avoid accidental touch** off preserves that choice but disables the backup
and its editor. See [touch-stop backup](brew-by-weight.md#when-touch-protection-delays-a-stop).
Home and Settings can therefore display different BBW values.
Home Assistant follows the same scopes: BBW is session-only, No-scale BBW is a
shared policy, fast and slow shot switches persist only in the active preset
without changing its target or learned values, and the scale-loss backup, cup
protection and accidental touch are shared settings.
Save remains bound to the preset whose fields were loaded into the form; an
asynchronous active-preset change cannot redirect those values to another recipe.
The Linea Micra brew target accepts 80.0–100.0 °C in 0.1 °C steps and defaults
to 93.0 °C for new and factory-reset recipes. Duplicating a preset copies this value;
disabling the option or disconnecting the Micra account does not erase it. See
[Linea Micra settings](../settings/linea-micra.md).
With a connected Micra account and **Set the brew temperature in each preset**
enabled, a persisted active-preset change applies that value through the cloud.
A scale connection reapplies the active value. Shot or scale-connection activity
defers the request, rapid changes keep only the newest target, and success is
reported only after the Micra dashboard confirms the same temperature.

## Factory recipes

| Value | Double · Adaptive | Single · Adaptive | Double · Fixed | Single · Fixed |
| --- | ---: | ---: | ---: | ---: |
| Dose | 18 g | 9 g | 18 g | 9 g |
| Target yield | 36 g | 18 g | 36 g | 18 g |
| Shot time window | 27–36 s | 23–32 s | 27–36 s | 23–32 s |
| Extend fast shots: extend by up to | 6 g (42 g) | 3 g (21 g) | off (6 g saved) | off (4 g saved) |
| Shorten slow shots: shorten by up to | 3 g (33 g) | 1.5 g (16.5 g) | off (3 g saved) | off (1.5 g saved) |
| Fast and slow transitions | 6 s, Soft: Steep early (fast), Steep late (slow) | same as Double | 5 s, Even, Soft (saved) | 5 s, Even, Soft (saved) |
| Expected drip weight after the shot stops | 0.5 g | 0.5 g | 0.5 g | 0.5 g |

All four start with brew by weight and grind hints on, Max shot time 50 s,
Earliest weight stop 12 s, and no fixed scale-loss stop time. The Fixed
presets keep their fast and slow amounts saved but switched off, so they always
stop at the target — useful while dialing in a new coffee.

**Manual** keeps the Double · Adaptive values, but brew by weight, Extend fast
shots and Shorten slow shots are off, so the shot runs until you stop it. Its
card still shows the Double recipe for reference. Protections such as cup
protection belong to the whole controller, not to a preset, so choosing Manual
does not change them.

Every factory preset, Manual included, can be returned to these values with
**Restore** but never deleted.

If you update a controller that already has presets, Manual appears at the
end of your list after the update restarts it. It is added only when there is
a free slot and none of your presets is already called "Manual"; otherwise
your list stays as it is, and Manual appears after a later restart once a slot
is free and the name is available.

Learning follows the preset. New and factory recipes seed the offset from
the table above and use current/base α=0.30. Duplicating a preset copies the
baseline, offset and gain/provenance into an independent recipe with empty
candidate evidence. Reboot retains those saved values and rebuilds only the
transient evidence window.

Use a separate preset for each physical portafilter/basket setup. The learned
drip in grams, gain α and candidate observations belong to that preset; shots
from another preset do not train it. The candidate window is transient, while
offset/gain and the shot log are persistent. When you change the physical setup
under the same preset (another basket, a spouted or bottomless portafilter, a
different dose), the drip is not re-detected: use the drip field's **Set** key
to start it again.

**Set** on the drip field uses the value you enter from the next shot and keeps
it as the preset's starting point; if your next three shots agree on a different
amount, the drip moves there at once. **Set** on the learning-speed field
(0.01–1.00, step 0.01) applies a new speed at once and keeps the drip. Each key
appears inside its field while your value differs from the one in use; pressing
Enter does the same.
**Restore** on a factory preset returns the whole recipe, its drip and a
learning speed of 0.30, and starts a quick start; it is available only for
factory presets. A device factory reset additionally erases other settings and
history. See
[BBW learning](brew-by-weight.md#cutoff-behavior-and-learning)
for prediction, gain selection and reset behavior.

Example: duplicate Double · Adaptive, set a 40 g target and save. Loading
Single · Adaptive later does not overwrite the 40 g recipe or its learned
offset.

New history records capture the originating preset ID; CSV exports `preset_id`
alongside the cutoff metadata. Keep the ID-to-physical-setup mapping with exports;
see [history provenance](shot-history.md#read-a-result) for older records and ID lifecycle.

Related: [BBW](brew-by-weight.md), [Fast and slow shots](fast-and-slow-shots.md),
[scale-loss backup](auto-to-manual.md), [cup protection](cup-protection.md).
