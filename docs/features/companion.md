# Open Brew by Weight Companion

Open Brew by Weight Companion is an optional little server you run on a
computer at home (a NAS, a Raspberry Pi, a home server). It gives the
controller a hand: the controller keeps doing everything it does today, and
whatever the companion takes over simply comes back to the controller when
the companion is not there. You never have to change a setting when the
companion goes away or comes back, even in the middle of a shot.

Without a companion, nothing changes: the controller works exactly as it
does today.

## What it does today

- **Opens the Web UI over HTTPS** with a trusted certificate. Once paired, your
  phone can install it as an app, and the companion checks every Web UI file
  against a list the controller sends sealed, so a device on your network can't
  change what your browser runs. With the machine off, Shots and History stay
  available from the companion. This is
  what the companion did when it was called the HTTPS gateway.
- **Lets several screens use the Web UI at once.** Once paired, your phone,
  your tablet and your computer can each keep the Web UI open through the
  companion, on whatever page they like: Home on the tablet by the machine,
  Shots on your phone, Diagnostic on the computer. The controller keeps
  talking only to the companion, which shares everything out to your
  screens, so its radio stays free for your scale. This works with every
  machine.
- **Talks to La Marzocco for you** (Linea Micra). Normally the controller
  itself keeps a secure connection to La Marzocco's cloud open and reads every
  message the cloud sends. With the **La Marzocco through the companion**
  switch on, the companion takes over that work: it holds the cloud
  connection and passes the controller only what it needs, namely the
  machine's power state, the boilers, the brew temperature and automatic
  backflush. Temperature changes, waking the machine and putting it to
  standby still work exactly as before; they just travel through the
  companion. The controller then spends its Wi-Fi and processor on your
  scale and your shot instead of on the cloud. During a shot with a scale it
  holds the machine's updates back until the shot is over — the newest one
  arrives right after — leaving the radio to your shot.
- **Keeps the diagnostic log's history.** With the **Diagnostic log through
  the companion** switch on, the companion keeps a copy of the controller's
  log, so the Diagnostic page can page back beyond what the controller holds
  in memory, across restarts, and even while the controller is off. See
  [The diagnostic log's history](#the-diagnostic-logs-history) below.
- **Keeps every shot and activation.** With the **Shot archive in the
  companion** switch on, the companion copies each shot and activation,
  including the ones made while it was away, and keeps them after the
  controller lets them go. Opened through the companion, Stats and History
  show your whole history, even with the controller off. See
  [Your whole history](#your-whole-history) below.

- **Sends your notifications.** An app installed from the companion's address can
  [notify you when a shot ends](notifications.md). The controller decides what to
  say and when; the companion only delivers it, over its own internet connection.

If the companion stops, the controller reconnects to La Marzocco on its own.
During a shot with a scale it waits until the shot is over, so nothing
competes with your scale; without a scale it reconnects right away. A
backflush that loses its supervision stops safely, exactly as it does when
the cloud connection drops today. The controller also keeps its own
connection while the companion reports that La Marzocco is refusing its
reads, and hands the cloud back to the companion once it recovers.

## Several screens at once

Open the companion's HTTPS address on each phone, tablet or computer. Every
screen works on its own: it can show a different page, scroll History or
Shots with its own filters, and keep its own sign-in when the Web UI asks for
a password. Install that address as an app and it stays out of your way: an
installed app keeps its place and reconnects by itself every time you open
it, without the Reload button a browser page shows after sitting unused.

Shots and History are ready before you open them. With **Shot archive in the
companion** on, the companion answers them from its own copy of your history:
scrolling, sorting, filtering, exporting and opening a shot's chart ask the
controller for nothing. The controller tells the companion about a new shot,
a rating or a deletion as soon as it is saved, and the companion copies just
that. If its copy is behind (right after you rate a shot, for example), the
page waits a moment for it; if the copy cannot catch up within about three
seconds, that page comes from the controller instead, so a list is never out
of date. With the switch off, the pages come from the controller, which
remembers the last page it showed and answers with one small message when
nothing changed.

A few things stay one at a time, on purpose:

- **Admin unlocks for one screen at a time.** Unlocking Admin on the tablet
  locks it again on the phone, so only one screen can start the machine or
  change Admin settings.
- **Saving a form that changed meanwhile.** If someone saves the same network or
  webhook settings on another screen while you are editing them, your save is
  refused and the form shows what is saved now. Make your change again if you
  still need it. For date and time and the scheduled restart, the last save wins.
- **No secure session, no Web UI through the companion.** A paired companion
  sends everything to the controller sealed. If it can't open that secure session,
  for instance when the two run versions that don't match, its pages say so and name
  the controller's own address, which keeps working. Update whichever is behind.
- **Opening the Web UI directly takes over.** If someone opens the
  controller's own address instead of the companion's, every companion screen
  pauses and says the Web UI was opened directly on another device. **Reload**
  on any of them brings control back to the companion.

If the companion stops, its screens show that the controller is out of reach;
the controller's own address keeps working as before.

## Connecting the controller to a companion

1. Install the companion on a computer on the same network (see its README).
   It finds the controller by itself.
2. Home shows **Open Brew by Weight Companion "…" wants to connect**, with
   the companion's address. Tap **Review** to open **Admin → Companion**.
3. Open the companion's page on that computer: it shows a six-digit code.
   Type that code in Admin. On a Linea Micra, **La Marzocco through the
   companion** is on: leave it on to let the companion take over the cloud
   connection right away, or turn it off to keep the controller's own
   connection for now. Tap **Approve**. Admin shows the steps while the
   controller and the companion connect. A request waits ten minutes; if it
   runs out, choose **Pair** on the companion's page to ask again.

The companion opens the Web UI only once it's paired, because that's what protects
your password and sign-in on your network. Until then, its secure address takes
you to this same page, with the code and a link to the controller's Admin; once
you approve, it opens the Web UI by itself. The code is never filled in for you:
typing it is how you confirm that the companion asking is the one you are using.

If the companion has not found the controller, type its address in **Admin →
Companion → Connect to a companion at…** and tap **Open its page**. The
companion's page opens in a new tab: choose **Pair** there, then come back to
Admin for the code.

**Ignore** turns down a companion you do not want, and it stops asking.
**Stop ignoring** lets ignored companions ask again.

Pairing never sends your La Marzocco password in the clear: the controller
hands the account over encrypted, only to the companion you approved. The
companion signs in with its own La Marzocco installation, so the La Marzocco
app, Home Assistant, the controller and the companion can all be connected at
the same time.

## Switches

**Admin → Companion** shows the paired companion, whether it is connected and
since when (or since when it is not), and, on a Linea Micra, where the La
Marzocco connection runs right now: **Through the companion**, **Moving to the
companion**, or **Directly from the controller** (with what it is waiting for,
if anything). If the companion and the controller cannot talk because one of
them is much newer than the other, Admin says so: update the older one. The
Web UI still opens through the companion meanwhile, so you can update the
controller from there. Opened through the companion with **Shot archive in
the companion** on, Admin also shows its **Shot copy**: whether its copy of
your shots and of History is up to date or catching up, how many pages it
answered from that copy, how many it passed to the controller, and how many
times it asked the controller for missing records. It has one switch per job:

- **Web UI through the companion** (on by default): phones, tablets and
  computers can open the Web UI through the companion, several at once. Turn
  it off to allow only direct connections to the controller. If you turn it
  off from a screen that is itself using the companion, Admin warns you first
  and gives you the controller's direct address, because that screen loses
  access too once you save.
- **La Marzocco through the companion** (Linea Micra builds).
  - **On:** the companion connects to La Marzocco's cloud for the controller.
    The controller no longer keeps a secure cloud connection open or decodes
    its messages, so its Wi-Fi and processor stay free for your scale and
    your shot. Nothing changes in what you can do: power, brew temperature
    and backflush work the same. If the companion goes away, the controller
    connects on its own until the companion is back, without you touching
    anything.
  - **Off:** the controller always connects to the cloud and does all that
    work itself, exactly as it does without a companion. The companion
    deletes its copy of your La Marzocco account.
- **Diagnostic log through the companion** (on by default).
  - **On:** the companion keeps a copy of the controller's diagnostic log, so
    its history survives restarts and busy periods, and pages back beyond
    what the controller keeps in memory. It stays readable even while the
    controller is off. During a shot the controller sends nothing: events
    wait and are copied when the shot ends, so the scale's radio is never
    disturbed.
  - **Off:** the controller's log stays in its own memory only, exactly as it
    does without a companion. The companion keeps what it already stored.
- **Shot archive in the companion** (on by default).
  - **On:** the companion copies every shot and activation and keeps them
    when the controller no longer does. Stats and History opened through the
    companion show them all, and the app installed through it can show your
    shots and activations while the controller is off.
  - **Off:** the companion stops copying and keeps what it already has.
    With the controller off, the app offers no shots.

Save and Revert work like the rest of Admin.

## The diagnostic log's history

The controller keeps its diagnostic log in memory only: restarting it, or
simply a busy stretch, loses the oldest lines. With **Diagnostic log through
the companion** on, the companion keeps a copy, and the log becomes one
continuous history:

- Opened **through the companion**, the log on the Diagnostic page reaches
  back as far as the companion keeps it. Scroll up and older events load by
  themselves, across boots (each starts with a **Boot** row that stays pinned
  at the top while you read its lines; the reset reason is in the log
  itself) and through gaps named as such when events could not be copied
  while the companion was away. A divider marks each new day, and the
  **Level** and **Subsystem** filters apply to the whole history, so every
  page that loads shows matching lines.
- The bar on top of the log moves through time: **‹ Day ›** and **‹ Boot ›**
  open the previous or next day or boot on its first line, and the clock
  (**Jump to a time**) shows the log up to a date and time, read on the
  controller's clock as the log shows it. Its shortcuts go back 15 minutes or
  an hour, or open **Today** or **Yesterday** on the first line. Live updates
  then pause; stepping past the newest day or boot, or **Back to live**,
  returns to the newest events. After about 40,000 lines the view stops loading and points you to
  **Export**.
- **Export…** downloads a file with every level and subsystem, whatever the
  filters show. It offers what you're viewing (the default), the last 24
  hours, the whole history, or a custom range where the start, the end or
  both can be left open.
- Also through the companion, Admin's **Shot, activation and log history**
  offers **Clear log history**, which deletes the companion's copy after you
  confirm. The controller keeps its own recent log, which the Diagnostic log
  keeps showing. It needs the controller on, to confirm that Admin is
  unlocked on your screen.
- Opened **directly**, the log shows what the controller holds, with a note
  that the older history is in the companion.
- The companion's own page (its address, then **Companion**) shows the whole
  stored history with a level filter and an **Export everything** download.
- The history opens only for a browser the controller lets open the Web UI:
  with the Web UI password on, one that signed in through the companion.
  That browser keeps reading it while the controller is off, and even after
  the companion restarts. It is never shown over plain HTTP.
- The history is kept for about a month by default, in a size budget of
  about 50 MB; the oldest events go first. Both can be changed with the
  companion's `COMPANION_LOG_RETENTION_DAYS` and `COMPANION_LOG_RETENTION_MB`
  settings, and exporting is the way to keep anything longer.
- Forgetting the companion deletes its copy of the history too.

Diagnostic → Companion also shows the log stream's health: whether it is on,
when the companion last copied events, and how many events were reported
missing while the companion was away.

## Your whole history

The controller keeps your newest 1,000 shots and 1,000 activations. With
**Shot archive in the companion** on, the companion keeps a copy of all of
them, and keeps them when the controller no longer does:

- shots and activations older than the controller's newest 1,000;
- your earlier history after a factory reset: the controller starts counting
  again from #1, and the companion keeps what came before.

Everything the controller holds is also on the companion, once it has copied
it. What you delete is gone from both: the companion never keeps a record you
deleted (see [Deleting records](#deleting-records)).

The companion catches up by itself whenever it connects, so shots made while
it was off are copied too. Only one thing can be missed: if the controller
records more than 1,000 shots before the companion is back, the oldest of
them are gone before it can copy them, and the list says so where the gap
is. Nothing is copied during a shot, nor while the machine is busy (a rinse,
a backflush, a scale connecting): the controller and the companion do not
talk to each other at all until it is over, so the scale has the radio to
itself. A new shot is copied the moment the shot and its drip delay end, and
pages opened meanwhile come from the companion's copy.

**Through the companion**, Stats and History are one list. Sorting and
filters work on the whole history, newest or oldest first, by rating, by
preset, by scale and by result. Records only the companion keeps are marked:
in Stats their number badge is grey, in History their type badge has a
dashed outline. Hovering the badge, or a screen reader, says **Kept by the
companion**. Since
numbers restart after a factory reset, a list sorted by date shows a line such
as **History restarted on 3 Mar 2026** where an earlier history begins; in
other orders the badge's description names that date.

The averages and charts at the top of Stats also cover your whole history,
built by the companion. You can pick how many of your newest shots they
cover: 10, 50 and 100, plus 500 and 1,000 through the companion. Only the
choices that make a difference are offered: every size smaller than the
number of shots in the list, plus the first one that covers them all, and no
choice at all with ten shots or fewer. Filtering the list down to fewer
shots moves the selection to the largest size still offered. The figures
cover your whole history only once the companion holds the controller's
newest shot; until then, or while it cannot answer, Stats show the
controller's own figures with a note saying so, and offer 10, 50 and 100 only;
the size you picked comes back with the whole-history figures.
With the controller off they come from the companion's copy, or say they are
not available right now. Opened directly, the controller offers 10, 50 and
100 by the same rule. Preset figures, such as a preset's arrival line and
grind hint, and Home Assistant's statistics stay the controller's own.

**Opened directly**, Stats and History show what the controller holds, and
end with a note that the companion keeps a copy, including older records.

### Deleting records

Deleting is the same everywhere: one **Delete** in a record's **⋯** menu,
offered while Admin is unlocked on the screen you use, and **Clear shot
history** and **Clear activation history** in Admin (see
[Clear a history](../settings/factory-reset.md#clear-a-history)). Each asks you
to confirm and cannot be undone. A deleted record is gone from the controller
and from the companion alike; a record only the companion keeps is deleted
from the companion with the same **Delete**. Clearing a history empties all of
it, the older records only the companion keeps and histories from before a
factory reset included. Export first what you may want back.

However you opened the Web UI, the companion follows: a record deleted or a
history cleared with the Web UI opened directly leaves the companion's copy
the next time it copies from the controller, also after it was away. A record
deleted before the companion copied it is never copied. Records older than
the controller's newest 1,000 that left it by themselves are never taken for
deleted ones. A factory reset deletes nothing on the companion.

Through the companion, it deletes a record it alone keeps only after the
controller confirms, at that moment, that Admin is unlocked on the screen
asking; with the controller off, nothing can be deleted. The companion's own
page has no delete controls.

The companion's own page (its address, then **Companion**) has an **Archive**
section: how many records it keeps and how much space they use, each earlier
history with the date it restarted, any gaps, and **Export** (a download of
everything).

The archive opens only for a browser the controller lets open the Web UI, as
the log's history does, and never over plain HTTP.

### With the controller off

With the machine switched off, the app installed through the companion still
opens. Next to **Reload**, the screen that says the controller is out of
reach offers **Browse shots and activations** while **Shot archive in the
companion** is on (Admin → Companion) and the companion holds records; with
the switch off it offers nothing. The button opens **Shots** and **History**
read-only, with every record the companion keeps and the averages over your
whole history. Filters, sorting, rating, exporting, traces and
deleting come back with the controller: open **Home** and press **Reload**.

If the Web UI asks for a password, this works on a phone or computer that
signed in through the companion while the controller was on; the companion
remembers that sign-in and never asks for a password itself. A device it has
not seen signed in is told to sign in once with the controller on. With the
password off, every device can look. When your phone reaches neither the
companion nor the controller, the app has nothing to show until one is back.

## Forgetting a companion

**Forget** in Admin → Companion removes the pairing. The companion learns it
the next time it tries to connect and deletes everything it received from
the controller, including any La Marzocco account, the diagnostic log's
history and the shot archive, even if it was off when you tapped Forget. To
keep your records, export the archive from the companion's page first. Its
screens can still open the Web UI, one at a time, the way any HTTPS proxy
does.

A factory reset forgets the companion on the controller only. The companion
can no longer connect, but it keeps everything it received, the archive
included. Until you pair them again, its address can't open the Web UI: use the
controller's own address. Once paired, the companion picks up its archive where
it left off, with the history before the reset kept as an earlier history.

## Checking that it works

**Diagnostic → Companion** has two parts. **This screen** shows how the page
you are looking at was opened: directly from the controller, through the
companion (with its name and how many screens it serves right now), or
through another HTTPS proxy. **Companion** shows what the controller knows:
whether a companion is paired, its name, address and version, whether the
link is up or down and for how long, how often it reconnected, whether
**Web UI relay** is on and ready (with how many relayed requests the
controller refused), the **Diagnostic log** stream's state, and the **Shot
archive**: whether it is on and when the controller last sent records. On a Linea
Micra it also shows whether the La Marzocco
connection currently runs through the companion or directly, with the
reason. While it runs through the companion, **La Marzocco cloud →
WebSocket** says **Through the companion** and leaves out the figures of the
controller's own connection. Every link and route change is also written to
the Web UI log, with the reason a link went down.

If you turned off everything that needs the companion's connection, Admin can't tell
whether it is still running. It shows **Web UI relay with** the companion's name and
when the relay last worked instead of saying it is connected.

The server icon in the header shows the companion at a glance, as two racks.
Both lit means the connection is open. One lit means you are viewing this
page through the companion but its connection to the controller is down; the
Web UI keeps working. Both dimmed means the companion is not connected.
Tap the icon to see the server's name, the **Link** state, and whether the
**Web UI relay** is serving the page you are on.

On a Linea Micra, pairing a companion also changes the cloud icon in the header
from one cloud to two. Both lit means the machine's data runs through the
companion. A dimmed back cloud means the cloud link works, but the data still
runs directly from the controller. Both dimmed means the cloud link is off.
Without a companion you see a single cloud, on or off.

For the details, tap the cloud icon on any page: under **Connection type**, its
popup says **Directly from the controller**, **Through the companion**, or
**Moving to the companion** while the handover is underway.

For developers, the protocol is described in
[Companion link protocol](../COMPANION_LINK.md).
