# First setup and daily use

Use this guide after firmware installation and the applicable
[bench checks](MANUAL_TEST_PLAN.md). For equipment and wiring prerequisites,
start with [Hardware](HARDWARE.md).

## Connect for the first time

1. Keep the physical activator OFF (release a momentary button) and power the
   controller. Turn on the scale with no other app connected to it.
2. On your phone or computer, join the controller's
   [setup network](settings/ap.md#first-connection). The name is
   **`OpenBrewByWeightAP-`** plus eight characters unique to this controller
   (USB `AP_STATUS` shows the exact name) and the password is the device
   password (**`ineedacoffee`** from the factory). Stay joined even if your
   phone reports "no internet".
3. The setup page opens on its own within a few seconds; if it does not,
   browse to **`http://192.168.4.1/setup`**. It walks you through three short
   parts, and a bar at the top shows where you are:
   - **Your controller.** A new controller is switched off and leaves the
     machine working as usual, so the page first asks how you want to brew:
     keep it **as usual**, or **stop shots by weight** and pick a starting
     recipe. Then you choose your own **password**, typed twice. The factory
     password is the same on every controller, so setup always replaces it;
     from now on you use yours to unlock Admin and to join the setup network.
     **Save and continue** stores your brewing choice and your password right
     away, and a note confirms it. Your phone stays connected.
   - **Wi-Fi** (optional). Choose your home network, type its password, and
     select **Connect**. This only tests the network: the page tells you when
     the controller got in, and **Save Wi-Fi** keeps it. Picked the wrong one?
     Choose **Wrong network? Choose another** before saving. A tested network
     you do not save is forgotten after ten minutes, and the page shows the
     time left. If your phone drops off the setup network meanwhile, join it
     again and the page picks up where you were. **Continue without Wi-Fi**
     skips this part.
   - **Review and finish.** Everything is already saved here. The summary lets
     you change any item (each change is saved on its own), reminds you to
     switch your phone back to your home network, and shows the addresses to
     use from now on (`<name>.local` and the controller's new address), each
     with a **Copy** button. **Finish** (**Finish and restart** when you chose
     to stop shots by weight) closes the setup network. Closing the page
     without it loses nothing: the setup network closes by itself about a
     minute after the Wi-Fi is saved, and the controller restarts then if you
     chose to stop shots by weight.
4. Rejoin your home network on the phone/computer and open the controller at
   the address the setup page showed. On most home networks
   `openbrewbyweight.local` answers (a renamed controller answers at
   `<name>.local`). If that name does not open, find the controller's IP
   address in the router's connected-device list or via
   [USB NET_STATUS](SERIAL_CLI.md).
5. If the Web UI shows **Reload**, select it to claim this browser session,
   then open **Admin** and unlock it with the device password.
6. The password you chose during setup is shared by Admin and the setup
   network; change it any time in **Admin → Access**. To ask for it before
   anyone can use the Web UI, see [Web UI access](settings/web-access.md).

A wrong Wi-Fi password can be corrected right there: the reason appears at
the top of the card, your entries stay in place, and **Connect** tries again.
Nothing is saved until **Save Wi-Fi**, so a failed or unsaved test never
replaces a network that already works. If the paddle is on, the card asks
you to turn it off first.

You can also operate locally over the setup network while your client remains
associated. The [AP guide](settings/ap.md) explains its idle shutdown and
recovery.

## Web access

**Claim / Reload** gives one browser control of Home and Settings. Another client
can take the claim, and 15 minutes without using a control locks the page and
stops live updates. Scrolling does not keep it active; use Reload to resume.
The page stays active while a shot is running, so it never locks mid-shot. An
app installed from the companion's address behaves like an app instead: it
keeps its place and picks the session back up by itself every time you open
it, with no Reload button. The Admin page still locks after 15 idle minutes
there too, and a page opened directly at the controller's own address always
follows the Reload rule above.

Each row in **Equipment** shows the device's icon with a small mark: a green
check when it's ready, an amber exclamation mark when it needs a look, and a red
cross when it isn't ready or isn't connected. Beside it, the state is spelled out,
such as **Connected** or **Present**, and on the right you'll see the reading that
matters most: the weight on the scale with its name, the weight of the cup, and,
on a Linea Micra, the brew temperature or the minutes left to warm up. Tap a row
for all its details.

On your first visits, Home shows a few short notes, one at a time: whether to
ask for a password, how to connect a Linea Micra to your La Marzocco account,
a reminder to check your recipe before the first shot, and, once your scale is
saved, the option to use only that scale. Each note has a button that takes
you straight there and a **Dismiss** button. A dismissed note doesn't come back
in that browser, and the password note doesn't come back on any phone. Notes
also leave on their own once you're done: the recipe note, for example, goes
away after your first shot or when you change a recipe.

Home updates automatically when its information changes. In **Equipment → Scale**,
weight and timer follow the connected scale even outside a shot. Disconnecting
the scale clears those readings. **Equipment → Machine** follows the machine and
brew states; on a Linea Micra, it also shows power and **Machine cleaning
(backflush)**, with the same cleaning state shown in Diagnostics. Cleaning status
requires the Micra WebSocket connection and state observation to be enabled.

To follow a shot from a tablet or a phone propped next to the machine, tap
**Live** in the header, on any page. The dot on it pulses red while a shot is
running. You can also double-tap the shot card on Home (double-click it on a
computer, or select it and press Enter). The live view fills the screen,
always in landscape, with big weight, time and flow readings beside the same
graph as **How this preset cuts** in Settings: you watch the shot climb
through the fast, on-recipe and slow corridors toward the red cut line, and a
ring marks where the controller expects to stop it. The line takes the shot's
color as the forecast changes. Flow appears under the weight curve, in its
own strip below the graph on a tablet. With no shot running, the view shows
the active preset, ready for the next one, and after a shot it keeps that shot
until the next one starts. Tap the ×, double-tap the view, press Esc on a
keyboard, or use your device's back gesture to close it; you return to the
page you were on. While the view is open, your phone's screen stays on, so you
can watch the whole shot.

The header shows the controller's home-network Wi-Fi signal and its Bluetooth
link to the scale. More lit arcs or bars mean a stronger signal; amber means
weak reception. Tap Wi-Fi to see the connected network name and signal level;
tap Bluetooth to see the connected scale and its signal level. These indicators
update when reception moves between weak, medium, and strong; the popups show
the level rather than a fluctuating dBm reading. The scale uses
your friendly name when set; otherwise it shows the scale's model name when
recognized, or the name reported over Bluetooth. Tap the same
icon again, tap outside the popup, or press Escape to close it. When the
whole Bluetooth icon fades out, the scale is disconnected. A crossed-out Wi-Fi
icon means the home network is down. Dim bars or arcs under a visible symbol
mean a reading is not available yet, or the page cannot refresh it. The Wi-Fi
icon describes the home-network connection, even when you open the page through
the controller's own access point.

Use the rounded navigation bar to open Home, Stats, History, Settings,
Diagnostics or Admin. When there is enough room, it sits beneath the logo,
matches the content width and shows an icon beside each section name. As the
window narrows, the icons disappear first. When the names no longer fit, the
bar moves to the bottom and shows icons only. The current page is highlighted.
You can also drag that highlight sideways with a finger or the mouse: each
section opens as the highlight settles over it, and you stay on the last one
when you let go. The highlight moves like a drop of liquid: it stretches as it
travels and settles with a gentle bounce. If your device is set to reduce
motion, it jumps straight to the new section instead.
With navigation in the header, the gray area is visible at the top of the page.
The whole header stays in place while you scroll: the logo, the connection
indicators and the menu remain visible and the page's content passes
underneath them.
Diagnostics appears only when **Show diagnostic page** is enabled in
**Admin → Frontend**; hiding it gives the other buttons more room. While it is
hidden, its actions (profilers, resets, tare, heap map) also need Admin
unlocked and stay dimmed until you unlock it.
The phone header keeps its compact size at all times, with the logo and
connection icons always visible.

**While a page loads**: Home, Settings, Stats, History, Diagnostics, and Admin
show a loading view with the Open Brew by Weight mark, a moving wave, and
**Loading…**. Opening or refreshing the URL covers the whole screen. When you
switch pages using the menu or the browser's Back and Forward buttons, it covers
the content below the header, leaving navigation available.
It also appears when you return to a page you have already visited. Controls
appear directly in their current positions, without animating during loading or
the fade. As soon as the page's data is ready, the loading view fades away in a
quarter of a second.
Control animations work normally once the page is visible. Stats, History, and
Settings use this single loading view rather than separate loading animations
inside their sections.
On first opening a page, its content and fresh data load together. Returning to
a page reuses its content and controls with current data. Stats and
History request their current status and records together, and the loading view
waits for both before fading.
If loading fails, the page shows an error. If this browser loses its claim, the
loading view hands over to **Reload**.

**Add to home screen** installs the page like an app. On iPhone or iPad, open
the controller page in Safari, tap **Share**, then **Add to Home Screen**; from
then on it opens full screen with the Open Brew by Weight icon — the brand mark
on the brand colour, edge to edge, so it fills its place on the home screen
without a white frame around it. On Android, open the controller page in Chrome,
tap the **⋮ menu**, and choose **Add to Home screen**. Because
the controller is reached directly at its local address, browsers do not offer
an automatic install prompt — the menu option above is the installation. The
icon is taken from the page when you add it, so an icon added before a firmware
update keeps the artwork it was installed with: remove it and add the page again
to pick up the current one.

**Out of reach**: when the page opens but the controller cannot be reached —
for example while you are away from home — it shows the Open Brew by Weight logo with
an out-of-reach message and a **Reload** button instead of a raw error. The same
message appears if the connection drops while the page is open and stays down;
a brief drop — say, right after unlocking the phone — reconnects on its own, and
the page resumes by itself once the controller answers again. When the page opens
straight into the message, check that the phone or computer is connected to the
same network as the controller, then press **Reload**. Right after a firmware
update the message can appear while the controller restarts; wait a moment and
press **Reload**. If the browser has no saved copy of the page yet, it shows
its own cannot-connect page instead. Over the controller's plain `http://`
address the browser keeps that saved copy for about a day after the last
successful open; to open the app while the controller stays unreachable for
longer than that, reach it through a reverse proxy that serves it over HTTPS,
which lets the browser keep the whole app until the next firmware update.

**Admin unlock** is an additional password check for privileged actions.
The **Firmware** section in Admin holds a master switch that can turn every
brewing feature off and leave the controller as a transparent pass-through;
see [firmware mode](settings/firmware-mode.md).
It stays renewed while Admin is open, or expires 15 minutes after the last
privileged action. **Lock** closes it immediately without releasing the browser
claim. Most configuration changes require an idle machine.

Open any Web UI page to set up the time zone from this phone or computer when
the controller is idle; unlocking Admin is not required. Setup continues until
the zone has been saved successfully. In **Admin → Date and time**, automatic
mode follows the device using the UI. Turn it off to choose a zone manually,
check the preview, and save your settings. See
[Time zone and NTP](settings/wifi.md#parameters-and-behavior).

**Settings** and **Admin** group their options into rows you tap to open. A
closed row shows its current choice on the right, such as `18 → 36 g · 1:2.0`
for the recipe, `Max 50 s` for time limits, your Wi-Fi network, or `Off`, so you
can check your setup at a glance. The text changes as you edit, before you save.
While Admin is locked, its unlock row stays open so you can type the password
right away.

**Diagnostics** uses the same rows for its readings: open only the sections you
want to watch, such as Scale or Wi-Fi. Press and hold a row's title to copy that
section's readings, for example to paste them into a bug report; open the row
first to include its values.

**Settings** saves every change by itself: switches and lists right away,
typed numbers and text once you pause or leave the field, and only the settings
you changed are sent. A value that conflicts with another one is flagged and
saved as soon as you fix it; if the controller turns a change down, the field
keeps your value and a red message explains why. If you try to leave the page
while a change is still being saved, your browser asks first.

Save buttons in **Admin** stay dimmed until you change one of their values. When you save, the small icon on the button turns into a spinning
ring while the controller applies the change. A successful save dims the button
again; if saving fails, the button remains available so you can retry and the
red message bar explains the error. Every save button has a smaller revert
button beside it with a circular arrow. It lights up together with the save
button, and tapping it asks for confirmation and then puts the section's fields
back to their last saved values, so you can walk away from edits you did not
mean to keep. If you try to close, reload or leave the page while a section
still has unsaved changes, your browser asks before discarding them. Safari on
iPhone and iPad doesn't show this warning, so save before switching away there.

Remote start/rinse are disabled in default firmware. Admin unlock does not
enable them. Remote Stop is privileged; physical controls remain available
according to the selected [paddle](settings/paddle.md) or
[momentary](settings/momentary.md) behavior.

## Connect the scale and choose a recipe

1. Check Home for a connected scale and a fresh weight reading. A Bluetooth
   connection alone is not sufficient for automatic weight control.
2. On a new controller, **My scale first → First detected** adopts the first
   compatible scale that connects successfully and then prefers it. To replace
   it or use several scales, follow [Scales](settings/scales.md).
3. In **Settings → Presets**, tap **Double · Adaptive** or **Single · Adaptive**
   (or a Fixed preset to always stop at the target). For your own recipe,
   duplicate a preset and change its target yield and shot time window; each
   change is saved to that preset as you make it. [Presets](features/presets.md) lists the factory values.
4. For a latch/paddle installation, start with **Natural**, the default mode.
   Momentary builds show **Switch** instead; use their
   [button and sensor settings](settings/momentary.md).
5. Check **Quick rinse**. Some machine profiles, including the La Marzocco
   Linea Micra, ship with it on: a quick paddle ON→OFF then runs a short
   rinse instead of starting a shot. Turn it off until you have tried its
   [gesture deliberately](settings/quick-rinse.md).

## First shot

With a usable scale, BBW enabled, the factory Double preset, Natural paddle
mode, and Quick rinse off:

1. Place your cup on the scale.
2. Move the paddle ON. The controller tares the scale and starts the timer.
3. Leave it ON while brewing. The nominal goal is 36 g; the controller
   compensates for dripping, and a [fast or slow shot](features/fast-and-slow-shots.md)
   may finish a little above or below it.
   To finish early in Natural mode, move the paddle OFF.
4. After automatic stop, return the paddle to OFF before starting again.
5. Leave the cup on the scale through the drip delay (default 3 s). The final
   weight then updates Last Shot and, if eligible, history.

While brewing, a card at the top of Home follows the shot live — weight
against the goal, elapsed time, flow, and the first drop. After the stop it
keeps the result: the weight, time chips and pills such as **FAST**,
**ON RECIPE**, **SLOW** or **MAX TIME**, plus **EXTENDED**, **STOPPED EARLY**,
**CHANNELING** or **NO SCALE** when they apply. A manual stop shows how the shot
ended instead.
While brewing, the card's time follows the controller's clock and counts up
smoothly in tenths of a second, even between updates. If updates stop arriving
for 1.5 seconds, the time freezes and the card shows that its data is out of
date. When the shot ends, the card shows the controller's final duration. If
it differs from the running time by more than a tenth of a second, the number
fades out and back in with the final value; otherwise it simply settles on it.

On a **momentary** machine, press and release to start; another valid press
requests stop. Automatic stop uses a pulse, not a continuously open contact.
Without reed feedback, the running state is inferred and can be wrong.
Read [Stopping and time limits](settings/momentary.md#stopping-and-time-limits)
before relying on automatic operation.

## Common next steps

| Situation | What to do |
| --- | --- |
| Cup placed after starting | Place it within the [retare window](features/tare-retare.md); later placement is not automatically corrected. |
| Shot ends above or below target | Read how it ended in [history](features/shot-history.md), then check [fast and slow shots](features/fast-and-slow-shots.md) and the recent-shots chart under **Drip compensation** in your preset. |
| Scale unavailable before start | Follow [No-scale BBW](settings/no-scale-bbw.md); the default first attempt warns and blocks. |
| Scale disconnects during brewing | Weight stop pauses; the [scale-loss backup](features/auto-to-manual.md) may request a timed stop while reconnection continues. |
| Want to stop shots yourself | Tap the scale icon at the top of the page and choose **Manual** for now (picking a preset or restarting brings By weight back), or choose the **Manual** preset to make it your setup. |
| Cannot save a setting | Finish the cycle, return the activator to idle, reclaim the UI if necessary, and check Admin unlock for privileged settings. |
| Cannot reach the controller | Use [network troubleshooting](FAQ.md#network-and-access) before considering a reset. |

Next: [settings index](README.md#settings) · [troubleshooting](FAQ.md) ·
[updates](features/ota.md).
