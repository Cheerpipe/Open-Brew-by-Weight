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

- **Opens the Web UI over HTTPS** with a trusted certificate, so your phone
  can install it as an app and keep an offline copy of your shots. This is
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
  scale and your shot instead of on the cloud.

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

A few things stay one at a time, on purpose:

- **Admin unlocks for one screen at a time.** Unlocking Admin on the tablet
  locks it again on the phone, so only one screen can start the machine or
  change Admin settings.
- **Saving a form that changed meanwhile.** If someone saves the same Admin
  settings on another screen while you are editing them, your save is refused
  and the form shows what is saved now. Make your change again if you still
  need it.
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

You can also pair from the Web UI itself. When you open it through a
companion that is not paired yet, Home shows **Use the Web UI on several
screens** (and **Admin → Companion** shows the same offer). Tap **Pair**: the
companion asks the controller, Home shows its six-digit code, and **Review**
takes you to Admin, where you type that code and tap **Approve**. The code is
never filled in for you: typing it is how you confirm that the companion
asking is the one you are using.

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
them is much newer than the other, Admin says so: update the older one. It
has one switch per job:

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

Save and Revert work like the rest of Admin.

## Forgetting a companion

**Forget** in Admin → Companion removes the pairing. The companion learns it
the next time it tries to connect and deletes everything it received from
the controller, including any La Marzocco account, even if it was off when
you tapped Forget. Its screens can still open the Web UI, one at a time, the
way any HTTPS proxy does.

A factory reset forgets the companion on the controller only. The companion
can no longer connect, but it keeps anything it received until you erase its
pairing on the companion's own page.

## Checking that it works

**Diagnostic → Companion** has two parts. **This screen** shows how the page
you are looking at was opened: directly from the controller, through the
companion (with its name and how many screens it serves right now), or
through another HTTPS proxy. **Companion** shows what the controller knows:
whether a companion is paired, its name, address and version, whether the
link is up or down and for how long, how often it reconnected, and whether
**Web UI relay** is on and ready, with how many relayed requests the
controller refused. On a Linea Micra it also shows whether the La Marzocco
connection currently runs through the companion or directly, with the
reason. While it runs through the companion, **La Marzocco cloud →
WebSocket** says **Through the companion** and leaves out the figures of the
controller's own connection. Every link and route change is also written to
the Web UI log, with the reason a link went down.

For developers, the protocol is described in
[Companion link protocol](../COMPANION_LINK.md).
