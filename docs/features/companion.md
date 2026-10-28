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
- **Talks to La Marzocco for you** (Linea Micra). When the **La Marzocco**
  switch is on, the companion keeps the connection to La Marzocco's cloud and
  passes the controller only what it needs: the machine's power state, the
  boilers, the brew temperature and automatic backflush. Temperature changes,
  waking the machine and putting it to standby still work exactly as before;
  they just travel through the companion. The controller then spends its
  radio and processor on your scale and your shot instead of on secure cloud
  connections.

If the companion stops, the controller reconnects to La Marzocco on its own.
During a shot with a scale it waits until the shot is over, so nothing
competes with your scale; without a scale it reconnects right away. A
backflush that loses its supervision stops safely, exactly as it does when
the cloud connection drops today.

## Connecting the controller to a companion

1. Install the companion on a computer on the same network (see its README).
   It finds the controller by itself.
2. Home shows **Open Brew by Weight Companion "…" wants to connect**, with
   the companion's address. Tap **Review** to open **Admin → Companion**.
3. Open the companion's page on that computer: it shows a six-digit code.
   Type that code in Admin. **La Marzocco through the companion** is on: leave
   it on to let the companion take over the cloud connection right away, or
   turn it off to keep the controller's own connection for now. Tap
   **Approve**. Admin shows the steps while the controller and the companion
   connect. A request waits ten minutes; if it runs out, choose **Pair** on
   the companion's page to ask again.

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
since when (or since when it is not), and where the La Marzocco connection runs
right now: **Through the companion**, **Moving to the companion**, or
**Directly from the controller** (with what it is waiting for, if anything). If
the companion and the controller cannot talk because one of them is much newer
than the other, Admin says so: update the older one. It has one switch per
job:

- **La Marzocco through the companion** (Linea Micra builds): the companion
  keeps the cloud connection. Turn it off to go back to the controller's own
  connection; the companion deletes the account it received.

Save and Revert work like the rest of Admin.

## Forgetting a companion

**Forget** in Admin → Companion removes the pairing. The companion learns it
the next time it tries to connect and deletes the La Marzocco account it
holds, even if it was off when you tapped Forget.

A factory reset forgets the companion on the controller only. The companion
can no longer connect, but it keeps the La Marzocco account it received until
you erase its pairing on the companion's own page.

## Checking that it works

**Diagnostic → Companion** has two parts. **This screen** shows how the page
you are looking at was opened: directly from the controller, through the
companion (with its name), or through another HTTPS proxy. **Companion** shows
what the controller knows: whether a companion is paired, its name, address
and version, whether the link is up or down and for how long, how often it
reconnected, and whether the La Marzocco connection currently runs through
the companion or directly, with the reason. While it runs through the
companion, **La Marzocco cloud → WebSocket** says **Through the companion**
and leaves out the figures of the controller's own connection. Every link and
route change is also written to the Web UI log, with the reason a link went
down.

For developers, the protocol is described in
[Companion link protocol](../COMPANION_LINK.md).
