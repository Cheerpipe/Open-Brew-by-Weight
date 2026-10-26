# La Marzocco Linea Micra

Firmware built for the **La Marzocco Linea Micra** shows its controls in
**Settings → La Marzocco Cloud**, between Presets and Shared settings. Other
machine profiles omit the section and its diagnostics.

The integration reads the selected machine through La Marzocco's cloud
service. It does not use the Micra's Bluetooth connection, so scale discovery,
scale commands, and weight streaming keep exclusive use of the firmware's BLE
client.

## Connect an account and select a machine

Open Brew by Weight must be connected to your normal Wi-Fi network in station mode.
While the setup network is open or the controller isn't on your home Wi-Fi,
**Sign in** is unavailable and a note explains why.

1. Enter the email address and password you use in the La Marzocco app.
2. Choose **Sign in**, or press Enter in either field. The form makes way
   for a short progress list — **Signing in**, **Finding your machines** and
   **Connecting this controller** — where the current step spins and each
   finished one turns green. The section title shows **Connecting…**
   meanwhile.
3. If the account has one Linea Micra, that's it: the controller connects to
   it and saves the recommended options right away, without asking you to
   choose. This also happens if you reopen Settings while a single machine is
   waiting to be selected. If saving it fails, the machine is offered for you
   to select instead.
4. If the account has several, each machine is listed with its name and serial
   number. Choose the one this controller is paired with and select **Use this
   machine** (or press Enter), or **Cancel** to sign out again.

If the sign-in fails, the form returns and the line under **Sign in** says why in plain words — for
example, when La Marzocco doesn't accept the email and password or the account
has no Linea Micra — and the password field is ready for another try. The
password is cleared after every attempt.

Before an account is connected, only the sign-in form is shown. Once a machine
is selected, the form is replaced by the connected machine — its name and
serial number with a **(Change)** link, a coloured status pill, and your
account email with a **(Disconnect)** link. A short note confirms that the recommended
options are on; it goes away as soon as you change one of them.

The status pill shows the most important thing to know right now:

| Pill | Colour | Meaning |
| --- | --- | --- |
| **Ready** · Set to 93.5 °C | Green | The machine is on and up to temperature. |
| **On** · Set to 93.5 °C | Green | The machine is on; its readiness isn't known yet. |
| **Heating up** / **Steam heating up** | Amber | The machine is on and still reaching temperature. |
| **No water** | Red | The machine reports an empty tank. |
| **Standby** | Grey | The machine is in standby. |
| **Machine offline** | Amber | La Marzocco reports the machine itself as disconnected. |
| **Cloud unreachable** | Amber | The controller can't reach La Marzocco right now. |
| **Waiting for the clock** | Amber | Sign-in waits for the controller's clock to sync. |
| **Account not accepted** | Red | La Marzocco no longer accepts the saved account; disconnect and sign in again. |
| **Paused during Wi-Fi setup** | Amber | Cloud checks wait while the setup network is open. |
| **Power state off** | Grey | **Show power state** is off. |
| **Power state unknown** | Grey | No reliable reading yet. |

The brew temperature is added while the machine is on, and **Paused during
shot** while cloud requests wait for a shot with a connected scale. Hover over the pill to read the full
explanation of a problem.

### Recommended options

A new controller, or one that was factory reset, starts with every option on
except **Turn the Micra on with the scale**, which you switch on deliberately
because it starts heating the machine even when you're not next to it.
**Standby when the scale turns off** starts with a 15-second delay. Updating the
firmware keeps the options you already saved.

### Change the options

The options are grouped under **Machine power**, **Scale automations** and
**Presets**. Like everything else in Settings, each option is saved as soon as
you change it. **Sign in**, **Use this machine**, **(Change)** and
**(Disconnect)** are actions of their own and also take effect immediately.

The four options that rely on **Show power state** are listed under it. When
**Show power state** is off they are dimmed and can't be changed, but keep
their choices.

### Change machine

Choose **(Change)** next to the selected machine to see the machines on your
La Marzocco account, pick the one this controller should follow, and select
**Use this machine**.
You don't need to sign in again, and your option choices carry over to the new
machine. Until you confirm, the controller keeps working with the machine it
already has; **Cancel** takes you back without changing anything.

If your account has only one Linea Micra, the list shows just that machine and
there is nothing to change. The controller asks La Marzocco for your machines
only when you choose **(Change)**; opening Settings never does it on its own.
If La Marzocco does not answer, it tries again a few times over about 20
seconds before showing an error.

### Disconnect

Choose **(Disconnect)** next to your account email to remove the saved credentials and selected machine and
stop cloud checks; the sign-in form returns. Your option choices are kept for
the next connection. Saving options does not sign in again, validate the cloud
account, or reload the machine list. A very long machine name is shortened to
48 bytes, and a machine without a name is listed by its serial number.

The email, password, selected machine, and device installation key are stored
in the controller so it can sign in again after a restart. Settings and
Diagnostics show the saved email as a read-only value; the password, installation
key, and short-lived access and refresh tokens are never returned by the Web UI
or diagnostics. The access token is reused and renewed after 50
minutes. Authentication, machine commands and the initial state read use the
cloud API, reusing its secure HTTP connection when possible; live updates renew
their connection with the current token. **Disconnect**
removes the saved account credentials, installation key, selected machine,
cached list, in-memory session tokens, and cloud connection.

Scheduled reads and cloud failures do not disable **Disconnect** or the
Micra options. Disconnect only forgets the saved integration; it does not send
a live command to the machine.

The cloud interface used by the La Marzocco app is not a documented public API
and can change independently of this firmware. A cloud outage or API change
affects Micra monitoring, but never scale operation, paddle handling, relay
safety, or local shot control.

## Monitor machine power state

Enable **Show power state** to follow the machine through live updates from
the La Marzocco cloud. Each time the live connection starts, the controller
also reads the current state once, so Home is right from the start; a
reconnect repeats that read, and newer power updates take priority over it.
Earlier firmware also offered a periodic check every 30 seconds; a controller
that used it keeps it until you next choose **Save La Marzocco settings**,
which switches it to live updates.

Both wait for station Wi-Fi, a synchronized clock and a closed setup access
point. During a shot with a connected scale, and while the controller finds and
connects to a scale, WebSocket monitoring pauses so Bluetooth has the radio to
itself: the live connection stays open, but La Marzocco stops sending updates
until the pause ends. Updates then resume and the controller reads the current
state once, without reconnecting. Without a connected scale, using the paddle
keeps both WebSocket monitoring and API reads available, whether you brew a
shot, rinse or turn the machine on. If a shot with a scale is classified as a
quick rinse, the pause ends immediately; losing the scale also allows
monitoring to resume. Backflush gestures keep monitoring available with or
without a scale, and a backflush in progress keeps receiving updates while a
scale connects. If monitoring needs a new connection during a shot with a
scale or while a scale connects, it waits until that activity and the scale's
communication pause have ended. Wi-Fi and the local Web UI remain available.
Micra API requests, including sign-in, Refresh and machine commands, wait
through scale connection. During a shot with a connected scale, reads wait.
Starting any shot cancels pending power commands, including shots without a
scale. Requests already in progress are canceled
by the cloud worker as soon as it observes the pause; local control never waits
for that cancellation.
Paddle wake gestures and starts blocked by a guard keep observation connected;
they do not count as shots or rinses.

Home shows **Power** as **ON**, **OFF**, or **—** when unknown.
An active local estimate appears as **ON - Optimistic** or **OFF - Optimistic**.
If observation pauses during that estimate, its effective value stays on screen
with stale quality until a fresh power update arrives after reconnection.
Connection or temperature updates alone cannot confirm power.

Diagnostics → Machine distinguishes the last observed mode from its quality:

| Micra response | Displayed state |
| --- | --- |
| `StandBy` | OFF |
| `BrewingMode` | ON |
| `EcoMode`, missing or unsupported mode | UNKNOWN |

In WebSocket mode, the initial API reading stays current while the subscription
remains healthy, even if the machine sends no power update for several minutes
or hours. The next power update replaces it. Diagnostics shows the source beside
**Mode**, for example **StandBy (API)**, **StandBy (WebSocket)** or
**StandBy (API via WebSocket)**. The last label means the initial API reading
requested when the WebSocket subscription starts; a later manual API read shows
**API**. The sample age keeps increasing; unrelated messages do not reset it.
An explicit machine-offline report makes the retained value stale. When the
machine comes back online, La Marzocco sends its full state over the
WebSocket, so no extra API reading is needed.

In API mode, power evidence becomes stale after 30 seconds. API failures use
four attempts with 3, 6 and 9 second delays. Regular API polling resumes its
30-second cadence after exhaustion and reports communication-error quality and
UNKNOWN unless a paused value is being retained. A failed WebSocket initialization does not
start recurring API polls or discard valid power updates already received.
WebSocket losses retain the last effective value as stale and retry with the
same delays, followed by a 60-second cooldown after exhaustion. In both modes,
rejected credentials (for example after changing the La Marzocco password)
stop automatic sign-in attempts so the account is not locked by repeated
failures. Reconnect the account with the new password, or use
**(Refresh)** once the password is fixed, to resume.
Pauses and network recovery preserve this authentication stop; they do not
restart automatic sign-in attempts.
**(Refresh)** adds a gated dashboard read. It follows the same shot, scale,
clock and post-wake rules; it does not enable continuous API polling in
WebSocket mode.

**Machine cleaning** is available through WebSocket only. It shows **Inactive**,
**Waiting for paddle**, **Cleaning in progress**, or an unknown reported value.
The row identifies its last update and marks retained values stale when the
stream is paused or the machine is offline. API mode shows that cleaning is
unavailable. A cloud cleaning update never starts the machine or turns an
existing shot into backflush. Transitions during a shot pause may be missed.

## Boiler readiness

While the machine is on, La Marzocco's cloud reports whether each boiler is
still warming. Home uses that instead of assuming: the machine row reads
**Heating up** while the brew boiler heats, **Steam heating up** once shots
are possible but the steam boiler is still below temperature, and **Ready**
only when both boilers report ready. When the water tank needs refilling the
row reads **No water**, and while a wake-up is still being confirmed it
shows a plain **On** rather than claiming readiness.

Next to that state, the row shows how many minutes are left while either boiler
warms up, using the machine's own estimate. Once the machine is ready, it shows
the brew temperature the machine is set to instead. When there is nothing to
show, the row shows a dash with a short note: **Turn on to brew** while the
machine is off, **Refill the tank** when it needs water, and **Starting up**
while a wake-up is still being confirmed. The Micra doesn't
share its live boiler temperature, so Home never shows how hot it is right now.

The open machine panel adds one row per boiler. Each shows its state and,
while heating, the machine's own estimate of the time remaining, for example
`Warming up · ready in ~6 min`. The estimate counts down on the screen without
extra network traffic. While the brew boiler warms up, a notice at the top of
Home says the same, for example **Machine warming up · ready in ~6 min**, so you
don't need to open the panel. The steam row shows the steam level set in the app
(level 1–3), because the Micra controls steam by level rather than by
temperature; the current temperature of either boiler is not available through
the cloud service.

Diagnostics groups every machine-reported state under **Linea Micra**:
readiness, power state, observed mode, both boilers (with the brew target
temperature and remaining estimate), water, cleaning, and observation quality.
Connection and account details stay in **La Marzocco cloud**.

These states are informational. Nothing about paddle handling, guards, brew
control, or how the machine is turned on and off changes with them. In API
mode the same states arrive with each dashboard read instead of as live
pushes.

### Automatic backflush

With firmware enabled, WebSocket monitoring can supervise the automatic
backflush selected in the La Marzocco app. Follow the machine's cleaning
instructions and prepare the machine before requesting the program.

1. Leave the paddle OFF and request automatic backflush in the app.
2. Wait until Home reports **Backflush ready · paddle to ON**, then move the
   paddle ON within the opportunity offered by the app.
3. Home first shows **Checking backflush**, then **Backflush running**
   when the machine confirms the program. Return the paddle OFF after it ends.

The controller does not reproduce the app's start countdown. Until the program
is confirmed, the usual 60-second protection applies. A qualified confirmation
allows at most **180 seconds from the original activation**, including the time
spent checking. The allowance ends as soon as the controller receives a state
outside backflush in progress. It also opens the activation circuit on loss of
supervision, a safety stop, or paddle OFF. The circuit opens at 180 seconds even
if the reported program is still running.

If the request ends before confirmation, the circuit opens and History records
**Other**. If the connection was just established, or a previous request was
interrupted, Home may ask you to cancel and request backflush again in the app.
That prevents an old request from receiving the longer allowance. An
unconfirmed or ambiguous activation never receives 180 seconds. When the
physical paddle remains ON after any stop, return it OFF before trying again;
restoring the connection cannot restart the machine.

Disabling observation, switching to API monitoring, or changing the connected
account or machine ends supervised backflush. Return the paddle OFF before
starting again. The previous cleaning request does not keep ordinary operation
blocked after that configuration change; the normal 60-second protection applies.

Backflush works without a scale. A connected scale keeps receiving weight, and
can reconnect during the program; backflush does not tare it, run its timer, or
add a shot to Stats. WebSocket monitoring continues throughout. Temperature
changes wait while cleaning is pending or active, and pending automatic machine
power commands are canceled. A start can be refused if a machine command is
still finishing, the cloud session needs renewal, or the controller is busy
with the cloud connection at the very moment you move the paddle. The machine
never waits on the cloud: return the paddle OFF and move it ON again.

History records a confirmed activation as **Backflush**, including an
interrupted one. Its duration is the time the activation circuit was enabled;
the record does not certify a successful cleaning. Cloud updates can arrive
late, so opening follows the received state, manual stop or local safety limit,
not a guarantee of simultaneous physical completion. With firmware enabled,
API monitoring and other machine profiles retain their normal 60-second cap.
[Compatibility mode](firmware-mode.md) mirrors the physical paddle without a
duration cap and does not supervise automatic backflush.

### Cloud diagnostics and logs

**Diagnostic → La Marzocco cloud**, immediately above **Compile flags**, shows the saved
account email and the active selected machine. The email is visible to anyone
who can view diagnostics. The panel also shows the most recently completed
cloud call: its start date and time in UTC, API name and HTTP method, result,
HTTP status when available, and duration in milliseconds. Duration helps spot
slow cloud connections. Before the first call, the panel reads **None yet**.
WebSocket connection state appears separately, including whether the live
connection is receiving updates, parked with the connection kept open, or
closed, along with retry time, message/power/pong ages, payload receive and
transmit rates, totals, errors and stop latency.
Two counters distinguish **Planned connections** from **Unexpected
reconnections**. Planned connections include the normal initial
connection and resumes after maintenance, account or monitoring changes, and routine
session renewal. Unexpected recoveries follow Wi-Fi loss, cloud or protocol
errors, or connection and message timeouts, even when the controller closes the
socket to recover. A recovery remains unexpected if a planned pause occurs
before reconnection. Only connections that reach the cloud subscription count;
failed attempts do not. Both counters reset when the controller restarts and
describe WebSocket monitoring, rather than individual HTTP API calls.
Payload byte counts exclude TLS and network overhead.
Connection setup allows up to ten seconds for each transport operation, so a
slow secure connection has time to complete. A connection error adds a warning
to the log with HTTP, TLS and socket error codes, without credentials or payloads.
The last completed HTTP call stays visible during the next request; this history is
kept until the controller restarts, including after Disconnect.

Each request logs its method and route when it starts, then its API name,
matching request ID, duration, HTTP status, and result when it finishes. These
entries use the existing serial and Web logs and respect their configured log
levels. Successful calls use **Info**; cancellations and temporary connection,
session, rate-limit, or server failures use **Warning**. Permanent rejections,
invalid or oversized responses, and request setup failures use **Error**.
Routes replace the machine serial with `{serial}`; logs never contain account
credentials, tokens, or response bodies. The transient `HTTP_CLIENT` header-wait
warning is hidden from serial output. Use the final result to tell whether the
call failed, even if the server returned HTTP 200.

UNKNOWN is treated like ON for paddle behavior. It never qualifies a wake
gesture, so brewing and rinse behavior remain unchanged when no confirmed ON
or OFF observation is available.

## Machine-aware scale search

Two switches directly under **Show power state** tie scale
discovery to the machine's power state:

- **Find the scale faster while the Micra is on** — searches more actively for a
  scale while the machine is on, whatever the saved scan mode is.
- **Search for the scale less in standby** — uses less Bluetooth
  scanning time while the machine is off; finding a scale may take longer.

Both start on for new installs and after a factory reset, and start off after
updating from firmware that did not have them; a saved choice is kept across
reboots and updates until you change it. Each option acts only in its own state:
enabling the ON option changes nothing while the machine is off, and enabling
the OFF option changes nothing while it is on. When no matching state is
available — monitoring off, no account connected, the state unknown, or an
unsupported reading — the saved [BLE scan mode](scales.md)
applies unchanged.

The options need **Show power state** on, because they read the
same observed state. With monitoring off both switches are disabled but
keep their saved choices. The options follow the same resolved state Home
shows, including optimistic ON/OFF estimates and values retained through a
temporary connection pause. There are no timers to configure or wait for,
and paddle or scale power events alone never change the search.

The override only changes how hard the controller searches for a scale; it
never stops discovery, powers a scale off, or changes the ESP32's CPU and
radio hardware policy (see [Power management](power-management.md)).

## Recognize paddle wake gestures

Keep **Paddle wakes the Micra** on to use a monitored OFF state. The
last confirmed OFF counts while it is current or stale, so the next physical
paddle ON is then treated only as the Micra's standby wake
gesture. The controller mirrors the paddle through its normal relay safety path,
but it does not start brew or rinse, evaluate or consume guards, command the
scale, play alerts, call brew webhooks, or add shot/rinse
history. The gesture itself never changes scale discovery; the optimistic ON
it produces can select Aggressive searching only when
[Boost scale detection when machine is on](#machine-aware-scale-search) is
enabled. Returning the paddle to OFF opens the relay and ends the gesture,
regardless of how long it was held. History adds one **Power ON** entry with
the gesture's date, time, and duration; Stats remains unchanged.

The OFF→ON edge immediately adds an optimistic ON overlay for at most 60
seconds—twice the normal read interval—and delays the next dashboard read for 15
seconds so the cloud can converge. The overlay makes the operational state ON
without rewriting the last cloud-confirmed OFF classification. **(Refresh)**
waits for the same deadline. A fresh matching WebSocket power update can confirm the estimate earlier.
A contrary update retains it until the delayed API reconciliation or expiry.
The first successful reconciliation read after that delay supplies the next
confirmed classification. A request
started before the edge cannot publish stale OFF or change the deadline. A
failed read does not extend optimism. A pause retains the effective value until
a fresh power observation arrives, even after the estimate expires. Paddle movement while the confirmed state is ON,
UNKNOWN, or an optimistic overlay is already live does not create or extend
optimism and follows the normal brew/rinse
flow.

This option is independent from monitoring. Turning monitoring off keeps the
saved wake preference, but the effective state becomes UNKNOWN, so wake
recognition is inactive until monitoring produces a confirmed OFF observation.

## Turn machine on when the scale powers on

Keep **Turn the Micra on with the scale** on to wake the Micra when
you switch the scale on. It is the mirror of the shutdown option above: the
machine turns on — the same wake the La Marzocco app performs — when the
scale comes back from its own power-off. A scale that merely reconnects after
a lost signal, and the first connection after the controller starts, never
wake the machine, so an ordinary reconnect never sends a cloud command. If the
live connection already shows the machine on, nothing is sent either. If the
cloud reports the machine offline (for example, unplugged), the wake is
dropped rather than kept for later, so the machine never turns on long after
you switched the scale on.

The machine is never woken while a shot or rinse is running; that power-on
is ignored completely and nothing is queued for later. Starting a shot or rinse
also cancels a wake that was still waiting to run. Because the command
travels through the La Marzocco cloud, a saved, connected account is
required: without one the option stays disabled and has no effect even if it
was on before the account was removed.

The command uses the same retries as the shutdown option: if the controller
temporarily cannot reach the La Marzocco cloud when the scale powers on, the
command stays pending and is retried, so the machine wakes once the
connection returns. The moment the cloud accepts it, the controller treats
the machine as already on, exactly like the paddle wake gesture, and the
next fresh matching power update confirms that view; a delayed dashboard read
can reconcile a contrary update.
Turning this option off or disconnecting the account cancels a pending
wake.

Turning the scale off and then quickly back on combines naturally with the
shutdown option and its delay: switching the scale off starts the shutdown
countdown, and switching it back on inside the window cancels the shutdown
while leaving the machine on.

## Turn machine off when the scale powers off

Keep **Standby when the scale turns off** on to put the Micra in
standby — the same state the La Marzocco app calls standby — when you switch
the scale off. It reacts only to the scale being switched off by its own
control: a scale that merely loses signal, stops reporting weight, or runs out
of range never turns the machine off.

The machine is never turned off while a shot or rinse is running. If the
scale is switched off during one, that power-off is ignored completely and
the shot finishes with its normal protections; nothing is queued for later.
Starting a shot or rinse also cancels a shutdown command still waiting to run.
If the live connection already shows the machine in standby, or the cloud
reports it offline, nothing is sent.

The option only works with scales that report their power-off to the
controller. Because the command travels through the La Marzocco cloud, a
saved, connected account is required: without one the option stays disabled
and has no effect even if it was on before the account was removed.

While the option is on, a **Standby delay** choice appears below it:
**Immediately**, **5 s**, **15 s** (the default), **30 s**, or **60 s**. With **Immediately** the machine
goes to standby as soon as the scale powers off. With a delay, the controller
waits that long first, so switching the scale back on inside the window
cancels the shutdown and nothing happens. The delay also covers an accidental
power-off: pick the value that gives you comfortable time to notice and turn
the scale back on.

Turning this option off or disconnecting the account stops any pending
shutdown. If the controller temporarily cannot reach the La Marzocco cloud
when the delay ends, the command stays pending and is retried, so the machine
goes to standby once the connection returns (a shutdown queued while the clock
is still synchronizing is also sent as soon as readiness clears). The saved
delay is kept with the option so it is restored the next time it is enabled.

The moment the cloud accepts the shutdown command, the controller treats the
machine as already off: the operational state reads OFF for at most 60
seconds—twice the normal read interval—while the last confirmed reading still
says ON, and the next dashboard read is delayed 15 seconds so the cloud can
settle. This mirrors the wake gesture, where the paddle ON is trusted before
the cloud agrees. The first successful read started after that delay replaces
the overlay with the confirmed state. If no read succeeds within the 60
seconds, the confirmed ON simply becomes the visible state again until the
next read lands.

## Turn scale off when the machine powers off

Keep **Turn the scale off with the Micra** on and the connected
scale switches itself off once the Micra is confirmed off — after the cloud
dashboard reports standby following a confirmed on state. A stale or
optimistic reading never triggers it, and it happens exactly once per
on-to-off transition, so a flaky connection cannot switch the scale off.

Only scales that accept a power-off command over Bluetooth are switched off.
See [Scales](scales.md#scale-power-off-support) for the explicit list. If the
connected scale does not support the command, nothing is written to it and
the log records a warning that the power-off was skipped; if no scale is
connected at that moment, nothing happens either.

The two directions never loop: this option runs only after the machine is
already confirmed off, so the disconnect that follows the scale powering
down cannot queue another machine shutdown, and the machine going to
standby through the shutdown option above only powers the scale off once.
Neither option acts while a shot or rinse is running.

After sending the scale shutdown command, the controller blocks any later
scale command on that connection. Its BLE library also blocks reads, scanning,
and reconnection for 3,000 ms, beginning before shutdown is submitted and
restarting the interval when the scale's disconnect is observed. Normal
reconnection resumes after that quiet period, without waiting for a second
advertisement.

Like every cloud option, it requires a saved, connected account: without one
the switch stays disabled. The account requirement covers the machine
observation; the scale command itself is local Bluetooth and works with the
connected scale regardless of brand.

## Brew temperature in presets

**Set the brew temperature in each preset** reveals a per-preset value from
80.0 to 100.0 °C in 0.1 °C steps. New and factory-reset presets start at 93.0 °C;
duplicates copy the source value. Turning the option off or disconnecting the
account keeps every saved preset value.

When a preset change has been saved successfully, Open Brew by Weight sends its target
to the selected Micra. Saving an already-active preset also sends the target
after persistence when the temperature changed. A newly connected scale sends
the current active target again, which covers sessions where the machine or
controller was unavailable when the preset was selected. Repeated triggers are
combined, and only the latest active target remains pending. When the live
connection already shows the machine at that temperature, nothing is sent:
the target is reported as applied without contacting the cloud. While the
La Marzocco cloud reports the machine offline, the target waits and is sent as
soon as the machine is back online.

Application waits while a shot or backflush is active, while a backflush request
is pending, and while the scale is connecting. These conditions cancel an
in-progress cloud request without affecting
the relay, BLE connection, or local shot control; the latest target remains
pending and is retried after activity ends. If a change is queued while the
clock is still synchronizing or station Wi-Fi is not ready, it is sent as soon
as those conditions clear, without waiting for a cooldown. Temporary cloud and
authorization failures use the same bounded retries and saved account session
as power-state monitoring. The integration never asks for credentials or registers a new
installation merely to retry a temperature.

The cloud command is complete only after a dashboard read reports the requested
target. Once the cloud accepts a change, delayed confirmation retries only the
dashboard read instead of resending the change. Status and diagnostics
distinguish a pending, running, confirmed, canceled, rejected, unconfirmed, or
communication-failed application from the Micra's last reported target, without
mixing that result into power-observation quality. Status also reports whether
the change was already accepted and whether another automatic attempt remains
scheduled.

Connection timeouts, rate limits, and temporary server failures remain pending
and retry after the cooldown, up to three rounds while the machine is online;
after that the attempt stops until the next trigger. An expired session gets bounded refresh/sign-in
attempts first; if authorization remains invalid, automatic attempts stop. A
redirect or other permanent request rejection also stops automatic attempts for
that trigger, avoiding repeated cloud requests. Selecting or saving a preset
again, or reconnecting the scale, creates a new trigger. Turning this option
off, disconnecting the account, losing station Wi-Fi, or entering setup AP mode
prevents writes; saved preset values remain unchanged. Temperature application
does not require **Show power state** to be enabled.

Factory reset removes the Micra cloud account, selected machine, installation
key, and RAM session. The options return to the
[recommended defaults](#recommended-options), and preset temperatures
return to 93.0 °C. See
[Factory reset](factory-reset.md) and [Presets](../features/presets.md).
