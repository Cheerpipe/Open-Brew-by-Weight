# Troubleshooting

Start with the symptom below. Defaults refer to fresh factory settings; a
saved recipe may differ. [First setup](GETTING_STARTED.md)
covers the normal path.

## Shot and scale

### The shot reached target but did not stop

Check, in order:

1. The active preset stops by weight: it is not **Manual**, and its **Brew by weight** is on.
2. Home shows fresh usable scale readings.
3. The [Earliest weight stop](features/cup-protection.md) time has passed.
4. A [fast shot](features/fast-and-slow-shots.md#fast-shots) is not deliberately running longer.
5. [Original paddle mode](settings/paddle.md#original) is not waiting for OFF.
6. On momentary firmware, the machine state permits an automatic stop pulse.

Use the stop action for your physical switch/mode if the result is unexpected.
[Auto paddle mode](settings/paddle.md#auto) does not interpret OFF as early stop.

### The shot ended above or below target

Read how the shot **Ended** on its Stats history card (or the pills on Home's
last shot card) before changing calibration.

| Result | Likely check |
| --- | --- |
| Above target, **Fast** / **EXTENDED** | [Extend fast shots](features/fast-and-slow-shots.md#fast-shots) raised the cut line, up to the preset's extra yield. |
| Below target, **Slow** / **STOPPED EARLY** | [Shorten slow shots](features/fast-and-slow-shots.md#slow-shots) lowered the cut line, down to the preset's reduced yield. |
| **CHANNELING** | A flow surge kept the cut line at the target. |
| **MAX TIME**, around 50 s on factory Double | Max shot time, well after the 36 s window end. |
| After scale loss (**NO SCALE**) | The [scale-loss backup](features/auto-to-manual.md) time is measured from shot start, not disconnect. |
| Ordinary stop followed by extra dripping | Learned offset and drip delay; see [BBW](features/brew-by-weight.md). |

Prediction, sample timing and dripping mean the weight at stop need not equal
the final stored weight. Fast and slow shot handling does not guarantee extraction quality.

### The scale disconnected

Weight stop pauses while reconnection continues. Three coherent readings on
the recovered link can restore it. The [scale-loss backup](features/auto-to-manual.md) and other
applicable limits still act. A steady unchanged weight is not a lost stream.

If the scale was absent at start, [No-scale BBW](settings/no-scale-bbw.md)
controls whether the attempt is blocked or manual. A scale arriving later
does not retroactively make every manual cycle an automatic one.

### Can I place the cup late or brew without weight control?

A cup placed within the [retare window](features/tare-retare.md) can be tared
automatically. Later placement is not covered. **Require cup to start** blocks
the start instead.

For a timer-only session, turn BBW off in Home. Tare/timer remain available
when supported; weight stop, late retare and Max shot time do not. Home's
session selection does not overwrite the saved preset.

### Does the 60-second limit always stop water?

It limits electrical relay closure. On paddle installations that normally stops
brewing. A momentary machine needs a stop pulse and valid state conditions;
without scale/reed evidence, a tap-started machine may not receive it. Read
[Momentary limits](settings/momentary.md#stopping-and-time-limits).

## Network and access

### I cannot find the controller's Wi-Fi

On fresh settings, the [AP](settings/ap.md) starts at boot. Its name is
**`OpenBrewByWeightAP-`** plus eight characters unique to this controller.
USB `AP_STATUS` prints the exact name. Forget any saved network named only
`OpenBrewByWeightAP`. With saved Wi-Fi, fallback begins after roughly 25 s
without association. Automatic AP shuts down after 3 minutes with no associated
clients and does not restart after a later successful home-network connection
drops. Use USB `AP_START` or reboot.

### The phone says the Wi-Fi password is wrong

It is the controller password: **`ineedacoffee`** on a new or factory-reset
controller, otherwise the one you chose during setup or in Admin. Phones and
computers often show that message when they cannot finish joining, not only
when the letters are wrong. Join the unique `OpenBrewByWeightAP-xxxxxxxx`
name from USB `AP_STATUS`, forget any unsuffixed `OpenBrewByWeightAP`, and
retry next to the controller. The scale may wait to connect until you finish
this setup network.

### I saved Wi-Fi and the old page never returned

Reconnect your phone/computer to the home network, find the controller's DHCP
address in the router or USB `NET_STATUS`, then open that address and claim
the UI within 3 minutes. The AP address cannot follow a change to a different
network. Unconfirmed changes revert; see [Wi-Fi](settings/wifi.md).

### Wi-Fi drops now and then

The controller reconnects by itself. To find out why it dropped, look for
`station connection failed` in **Diagnostic → Log**. That line tells you why
the router connection ended and how strong the signal was when it did:

- **beacon timeout**: the controller stopped hearing the router. This is
  usually a weak signal. Inside a machine body, anything around −75 dBm or
  weaker is marginal; move the router or an access point closer.
- **no AP found**: the network wasn't visible when the controller tried to
  join.
- **auth failed**, **4-way timeout** or **handshake timeout** while joining:
  check the Wi-Fi password, or the router's security mode.
- **deauthenticated** or **disassociated**: the router ended the connection,
  for example when it restarted, changed channel or removed idle devices.

Any other reason shows as a number only; include it when you report an
issue.

### How do I reach the controller the first time?

Once the controller has joined your home Wi-Fi, open
`openbrewbyweight.local` in a browser on any phone or computer connected to
the same network — there is no need to look up the IP address. The name comes
from the controller's default device name; if you renamed it in
Admin → Network, open `<name>.local` instead (spaces become hyphens, so
`Cafe Bar` is `cafe-bar.local`). See [Wi-Fi](settings/wifi.md#discovery-by-name).

### I cannot open `openbrewbyweight.local`

Usually one of two things is happening. Either the controller never joined
your Wi-Fi (or your phone/computer is on a different network) — check that
both are connected to the same network. Or your network does not allow the
discovery behind `.local` names (mDNS), which guest and hotel networks
commonly block. In that case, find the controller's IP address in your
router's device list and open that instead. See
[Wi-Fi](settings/wifi.md#discovery-by-name).

### A custom web address jumps to the IP address or stops working

The controller answers only to its IP address, `<name>.local`, and the plain
device name (also with `.lan`, `.home.arpa` or `.internal`), so a website cannot
reach it by disguising its own address. A page opened by any other name moves
to the controller's IP address; Home Assistant, scripts or a reverse proxy
using such a name get an error and need the IP address or the `.local` name
instead. The Diagnostic log shows each refused name. See
[Web UI access](settings/web-access.md#what-it-protects-and-what-it-doesnt).

### The browser shows its own cannot-connect page instead of "out of reach"

The out-of-reach screen comes from a saved copy of the app. Over the
controller's plain `http://` address, browsers keep that copy for about a day
after the last successful open, so after a longer absence the browser shows its
own error page until the controller answers again. If you want the app to open
while the controller is unreachable for longer, reach it through a reverse
proxy that serves it over HTTPS; browsers then keep the whole app until the
next firmware update. See [Out of reach](GETTING_STARTED.md#web-access).

### Controls are locked or another browser took over

Reload claims the Web UI. Admin unlock is a separate password check. Idle
timeouts, another browser's claim, and an active shot can each restrict
controls for different reasons. See [Web access](GETTING_STARTED.md#web-access).

### The Web UI asks me to sign in

Someone turned on the [Web UI password](settings/web-access.md). Enter the
device password, the same one used for Admin and the setup network. If a phone
that used to open directly now asks again, it was signed out: the password was
changed, **Sign out other devices** was used, or it was not used for 90 days.

### I forgot the password or cannot reach any interface

Use [USB commands](SERIAL_CLI.md) or
[physical recovery](EMERGENCY_RECOVERY.md). Resetting only the password over USB
preserves Wi-Fi; access recovery also forgets the network. Factory reset erases
much more. Compare the procedures before choosing one. Each of them also turns
the Web UI password off.

### Automatic tare does not happen after switching on

Check **Home → Cup → Cup tare** and the scale status. With a positive
weight already present at startup, remove the cup, let the empty pan settle,
and replace it. If the Bookoo zeroed a load already on its pan, wait for the
initial zero to settle, remove it, let the negative empty reading settle, then
place a different-weight load. If it returns near zero, readings were lost, or
the negative reading was present from the first connection, tare the empty pan
using the scale's button, reconnect the scale, wait for stable zero and replace the load. Stable-looking
weight alone is insufficient after lost readings or a failed tare; an unchanged
cup is not automatically retried. See
[outside-brew tare](settings/tare.md#outside-a-brew).

### I pressed tare on the scale and the controller did not retare

The controller cannot verify a press of the scale's physical tare button from
the documented weight notifications. A return to 0 g can also mean a previously
removed load was put back. Home may still say **Cup present** and **Tared**
because those are its last known states. Tare the empty pan using the scale's
button, reconnect it, wait for stable zero, then place the cup again. The optional accessory retare
handles one stable addition to a controller-tared cup before a shot; it cannot
identify physical button presses. See [Tare](settings/tare.md#outside-a-brew).

### The scale connects slowly or the UI stutters

Close other scale-connected apps. Check the saved preferred scale and
**Admin → Power management → BLE scan mode**. Factory default is **Balanced**;
Relaxed uses less scanning radio time and Aggressive uses more. Try Relaxed
if idle scanning hurts UI response. On a Linea Micra, the
[machine-aware scale search](settings/linea-micra.md#machine-aware-scale-search)
can ease scanning while the machine is off.
**Admin → Power management → Wi-Fi sleep** can also affect latency.
See [Scales](settings/scales.md) and [Wi-Fi](settings/wifi.md).

### Why is there no remote Start or rinse?

It is a deliberate safety decision. The controller is built for a trusted
home network, not the internet, and it does not treat that network as
hostile — the integration API, for example, is unauthenticated. Any device
on the network can reach it, so a page that could start a shot or a rinse
would let a compromised phone or a guest laptop set a hot, unattended
machine brewing. The web interface reads data and edits settings; nothing
in it can make the machine run water. Remote start and rinse are disabled
in default firmware; an explicit development build can enable them, but
Admin unlock alone cannot. Remote Stop remains privileged (Admin unlock),
because stopping only ends brewing — it never begins it.
See [build options](BUILD.md#5-build).

### Why can't I turn the machine on from the web interface?

For the same reason: nothing you can reach over the network can switch the
machine on. The firmware does power the machine on by itself in some
automations — on a Linea Micra it can wake the machine when you switch the
scale on — but each of those begins with a physical action at the espresso
bar, such as the scale's power button or the machine's own switch. It is
never a decision the firmware makes on its own, and never one it takes from
the network. See
[Turn machine on when the scale powers on](settings/linea-micra.md#turn-machine-on-when-the-scale-powers-on).

### The web interface only shows the Admin page

The [firmware mode](settings/firmware-mode.md) switch is off: the controller
is running as a transparent pass-through and every other tab is hidden. This is
how a new or factory-reset controller starts. Unlock Admin with the device
password, open **Firmware**, and turn **Enable Open Brew by Weight** on.

## Hardware and compatibility

### Can I use this firmware with a machine other than Rancilio Silvia Pro X or La Marzocco Linea Micra?

Yes, but first determine and review the correct configuration for that exact
machine and controller assembly. Identify whether its brew control is momentary
or maintained/paddle, whether reliable state feedback such as a reed sensor is
available, how the isolated normally-open relay must connect, and which factory
defaults match the machine's gestures. Then create complete
[hardware and machine profiles](BUILD_PROFILES.md) and pass the applicable
bench and manual tests. A similar connector, brand, or switch appearance is not
proof of electrical or behavioral compatibility.

- **Boards / wiring:** [Hardware](HARDWARE.md). A GPIO map is not a complete
  machine installation guide.
- **Scale models / missing timer or sound:** [compatibility table](../libraries/EspressoScaleBLE/README.md#scale-compatibility).
  Implemented and physically tested support are different.
- **No USB port:** default app CDC needs GPIO4 held to GND at reset.
  ROM download uses BOOT + RST; see [USB console](HARDWARE.md#usb-console-jumper).
- **LED or beeps:** the connection LED is not a brew or safety-ready indicator.
  Output depends on [Alerts](alerts.md), local buzzer and scale capabilities.
- **Change GPIOs:** create or select a reviewed hardware profile and rebuild;
  the Web UI does not configure safety-critical pins.

## Safety and diagnostics

After a watchdog/panic reset, the relay is forced open and the interrupted shot
does not resume. Persistent hardware/feedback faults may still block starting.
An open K1 cannot stop a welded contact; see [isolation](HARDWARE.md#isolation-must).

The **Diagnostic** page is always available at the address `/diagnostic`, even
when its tab is hidden from the menu (Admin → Frontend → **Show diagnostic
page** only shows or hides that tab) and in compatibility mode. Its
measurements keep recording either way. While the page is open, its
**States**, **Machine I/O**, and **Scale** sections — including the scale's
timer — update on their own several times per second, so readings such as the
relay, the switch, the cup, and the weight stay current without reloading;
every other section refreshes every few seconds. The page finishes loading
when those sections have arrived. New log entries appear automatically while
Diagnostic is open, and leaving the page stops its live updates. The log also
shows warnings and errors from the device's built-in system components — for
example, the exact reason a secure connection to the La Marzocco cloud
failed — so you can troubleshoot from the browser without opening the machine
or connecting a USB cable. If the
connection drops, the interface reconnects and reloads current readings.

In **NVS**, unavailable entry and namespace measurements appear as a dash,
so a failed measurement is not mistaken for an empty store.

**Diagnostic → Scale → Profiling** records what the scale actually sent and
what the firmware decided about it. Press **Start** before reproducing a
problem — for example a wrong tare or an unexpected cup event — and **Stop**
when done; **Download** then saves a plain-text trace and **Delete** removes
it. Recording continues until its capacity fills or you press **Stop**, keeps
running while you leave the page, and works with or without a shot in
progress. Every decoded weight is kept exactly as received — including
identical, negative, and rejected readings — together with the tare, cup,
touch, and first-drop decisions taken around it, so a support conversation
can point at one line instead of guessing. Starting a new recording replaces
the previous one, which is stated next to the buttons; a finished recording
survives a restart once it reports as saved. Losing power during recording or
saving can lose that session.

**Capture capacity used** shows how much recording space has been consumed,
including the space kept for the closing event. **Elapsed** is the time already
recorded. **Estimated time remaining** adapts to how quickly readings and events
arrive, so different scales can record for different lengths of time. It takes
a few seconds to appear, smooths brief changes in activity, and may increase
when fewer events arrive. If no records arrive for several seconds, the estimate
returns to **Estimating…**; recording stays active.
The percentage describes the capture, while **Saving…** and **Saved** tell you
whether it has been stored for a restart. Stopping early keeps the actual
percentage used rather than changing it to 100%.
The state also explains recording errors, such as insufficient memory or a
failed save. If saving fails and **Download** remains available, download the
trace before restarting to preserve the recording held in memory.

On a 16 MB controller, **Diagnostic → Misc → Coredump** shows how many complete
crash records are saved, up to two. Unlock Admin to download them. The browser
saves a `.tar.gz` file when gzip is supported, or a `.tar` file otherwise.
Extract it with `tar -xzf <archive.tar.gz>` or `tar -xf <archive.tar>`.
Each crash folder contains the raw core dump, a short address list, and a
manifest with the matching firmware ELF fingerprint. Use the ELF from that
exact build when interpreting addresses. A watchdog dump may contain several
tasks; the short address list describes only the panic callback's view.
The archive can contain passwords or other private data, so share it only with
someone you trust. Downloading does not delete records; before the archive is
built, each saved dump is checked against its stored fingerprint, and a record
that no longer matches is left out (the count drops accordingly). **Empty** asks for
confirmation and permanently removes them; a full factory reset does too.
An incomplete panic or a reboot loop before the record is copied can leave
fewer than two records. The screen reports an error if a temporary dump cannot
be archived. The 8 MB profile does not offer persistent crash downloads.

If Wi-Fi, the scale or the machine cloud connection drops out intermittently,
memory may be fragmented rather than short. **Diagnostic → HEAP → Heap map →
download** saves a `heap-map.csv` file listing every block of the controller's
internal memory, used or free, at the startup moments that decide its layout
(before and after Wi-Fi starts, the first cloud connection, the end of
startup) and right now.
Attach it to your report along with the diagnostic export. While a shot is
brewing, the file contains only the startup moments.

To report an issue, include firmware version, board and machine type, scale
model/firmware, exact gesture/settings, and redacted diagnostic export. The
export's `audit` section lists rare faults and anomalies the firmware kept
aside from the rotating log — useful when the problem happened hours or days
earlier. The same records appear in the **Diagnostic → Audit log** table,
which survives restarts on its own flash space: the newest records come first,
five at a time, with small paging buttons to reach the older ones. Each row
shows what happened in plain words (for example how low the memory fell or
which task held a lock too long), and tapping a row unfolds the complete
detail. A missing time means the device clock was not yet synchronized when it
happened. **Download**
saves the whole saved log as a text file, and **Delete** there (or the
`AUDIT_CLEAR` USB command) removes the records permanently. When a problem is
actively being hunted, **Hunt 30 m** admits a few extra debug events into the
audit log for thirty minutes so a rare sequence can be captured; the session
ends on its own and never changes how the machine is controlled. Never
include passwords or webhook secrets. [USB HEALTH](SERIAL_CLI.md#diagnostics)
and [shot history](features/shot-history.md) help explain the observed result.

A `stale scale event rejected` warning means an old weight update or scale
command was ignored. Its detail names the rejected item and reason; `gen(e/n)`
and `disc(e/n)` compare the event's link identifiers (`e`) with the current
link (`n`).
This can happen briefly after a scale disconnects or reconnects.

## Where to change it

Use the [settings index](README.md#settings) for parameter references,
[OTA troubleshooting](features/ota.md#session-start-troubleshooting) for update
errors, and [webhook troubleshooting](features/webhooks.md#troubleshooting)
for delivery problems.
