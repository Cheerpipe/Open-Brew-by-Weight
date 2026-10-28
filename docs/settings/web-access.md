# Web UI access

Out of the box, anyone on your Wi-Fi can open the Web UI, just like before.
If other people share your network — housemates, guests, kids — you can ask
for a password instead. The controller uses one password for everything: the
**device password**. There is no user name.

## Turn the password on

1. Open **Admin**, unlock it with the device password, and go to **Access**.
2. Switch on **Require a password to open the Web UI**.
3. If the device password is still the factory one (`ineedacoffee`), choose a
   new password and repeat it. The factory password is public, so it can't
   protect anything. Otherwise, type the device password to confirm you know
   it.
4. Tap **Turn on**.

The phone or browser you used stays signed in. Every other device now shows
a sign-in page. Access reminds you to keep the password somewhere safe: if
you lose it, the only way back in is the recovery steps at the machine.

The first time you open the Web UI, Home shows a short note that it is open on
your network, with a **Set up** button that takes you to this panel. **Dismiss**
hides the note on every phone.

## Signing in

The sign-in page asks only for the device password. Your password manager can
save it; it shows the controller's name so you can tell controllers apart.

- **Remember this device** (on by default) keeps you signed in for 90 days of
  not using it, even when the machine is switched off. Each visit extends it.
- Turn it off on a shared or borrowed phone: you stay signed in only until you
  close the browser or the controller restarts, and at most 12 hours.

Five wrong passwords in a row from one phone or computer pause its password
attempts for 30 seconds, including Admin unlock and firmware updates that use
the password. Other devices can still sign in meanwhile, unless four
different devices are all paused at once: then every device waits until the
oldest pause ends, so repeated wrong guesses cannot be spread across many
devices.

Up to 8 devices can be remembered at once; signing in on a ninth forgets the
one you used least recently. Signing in with the password also unlocks Admin
for 15 minutes. A remembered device opens the Web UI but asks for the password
again before Admin.

Only one window controls the controller at a time, as before. Opening the Web
UI on another device moves control there, and the first window offers
**Reload** to take it back without signing in again.

Through an [Open Brew by Weight Companion](../features/companion.md), your
phone, tablet and computer can all use the Web UI at the same time, each on
its own page. Each screen signs in on its own, and Admin unlocks for one
screen at a time: unlocking it on the tablet locks it again on the phone.
Opening the Web UI directly on the controller's own address still takes
control: every companion screen pauses and explains why, and **Reload** on any
of them brings control back to the companion.

## What it protects, and what it doesn't

The password keeps casual visitors out of the Web UI and stops them from
changing presets, settings, or the machine. The connection is not encrypted,
so it won't stop someone who can watch the traffic on your Wi-Fi. Keep the
controller on your own network, not a guest or public one.

Joining the controller's own setup network already proves the device password,
so the setup page never asks for it again.

The password does not protect the board itself. The firmware does not use
secure boot or flash encryption, so anyone who can open the machine and plug a
USB cable into the controller can read its saved settings, including your Wi-Fi
password, or install other firmware. The factory device password
(`ineedacoffee`) is public, so change it when you set up the controller.

Whether the password is on or off, the controller answers only when you open
it by an address that names it: its IP address, `<name>.local`, or the plain
device name your router may offer (also with `.lan`, `.home.arpa` or
`.internal`). This stops a website you visit from secretly reaching the
controller through a look-alike web address. Opening it by any other name, such
as a custom name you set up in your own DNS, sends the page to the
controller's IP address, and apps or scripts using that name get an error.

An [Open Brew by Weight Companion](../features/companion.md) asks to pair and
connects without the password: its request only asks, approving it needs
Admin unlocked, and from then on it proves the secret it shares with this
controller. The companion only connects from your local network. The
password still applies to every screen that opens the Web UI through it, and
the controller still tells those screens apart: five wrong passwords pause
only the phone or computer that typed them.

## Home Assistant

While the password is off, Home Assistant connects as before. While it is on,
Home Assistant needs its own access, which requires an integration version that
supports it; update the integration before turning the password on. You can
give that access in either of two ways when you add the controller (or when
Home Assistant asks again):

- **Type the device password in Home Assistant.** Home Assistant uses it once
  to get its own key and does not keep the password.
- **Approve it in the Web UI.** Home Assistant shows a four-digit code, and
  the Web UI shows **Home Assistant wants to connect** with the same code.
  Tap **Allow** only if the codes match. The request expires after 2 minutes,
  and it closes on its own if you type the password in Home Assistant instead.

If Home Assistant was already set up when you turn the password on, it shows
a "needs reauthentication" notice; answer it the same way. Its entities read
"unavailable" until then, and nothing is lost.

Home Assistant appears in the **Signed-in devices** list under **Admin →
Access**. **Disconnect** removes its access; it then asks again. Changing the
device password does not disconnect Home Assistant.

## Change the password or sign out

- **Change the device password** in the same panel. It also changes the
  password for the setup network and for firmware updates. While the password
  is on, every other device must sign in again; yours stays signed in. Home
  Assistant stays connected: if you change the password because it leaked,
  also tap **Disconnect** so Home Assistant has to ask again.
- **Sign out**, at the bottom of every page next to the firmware version,
  signs out the phone or browser you are using.
- **Signed-in devices** in Access opens a list of every phone and browser
  signed in, such as "iPhone · Safari", with when it was last used. Your own
  device is marked **This device**. Tap a name to rename it, for example
  "Ana's phone"; Enter saves and Escape cancels. Tap **Sign out** next to any other device
  and confirm in the same row; **Sign out other devices** at the end signs
  out all of them at once.
- **Turn the password off** by switching it off and typing the device
  password. Remembered devices are forgotten and Home Assistant goes back to
  connecting without a password. Admin still asks for the device password,
  so keep it somewhere safe.

## Open it over HTTPS with a reverse proxy

If you already run a reverse proxy at home, such as Nginx Proxy Manager, you
can reach the controller at your own HTTPS address, for example
`https://brew.example.com`. After your first visit over HTTPS, the browser
keeps a full copy of the Web UI on your phone: it opens instantly, shows up
even while the machine is off, and reconnects as soon as the controller is
back. Everything you see and
do still comes live from the controller. After a firmware update the phone
picks up the new Web UI by itself.

The phone also keeps its own copy of every saved shot and every entry in the
activation history, and refreshes it about half a minute after each brew or
rinse.
When the controller is out of reach, for example with the machine switched
off, tap **View saved shots** to browse that copy in **Shots** and
**History**. It is read-only until the controller is back: to reconnect,
open **Home** and press **Reload**, and rating, filters, exporting, and
deleting return with it. If the Web UI
asks for a password, the copy is kept only on devices where you chose
**Remember this device**, and signing out erases it.

Installed on the home screen, the app also offers shortcuts to **Shots**,
**History**, and **Settings** when you press and hold its icon, and tapping
the icon brings back the window that is already open instead of opening a
second one.

Give the controller a fixed IP address in your router first. Then, in Nginx
Proxy Manager:

1. Add a **Proxy Host** for your domain, with scheme `http`, the
   controller's IP address, and port `80`.
2. Switch on **Websockets Support**, which keeps the live readings flowing.
3. On the **SSL** tab, choose your certificate and switch on **Force SSL**.
4. On the **Advanced** tab, paste this, replacing `192.168.1.50` with the
   controller's IP address:

   ```nginx
   location / {
     proxy_pass http://192.168.1.50;
     proxy_set_header Host 192.168.1.50;
     proxy_set_header Origin http://192.168.1.50;
     proxy_http_version 1.1;
     proxy_set_header Upgrade $http_upgrade;
     proxy_set_header Connection $http_connection;
     proxy_buffering off;
     proxy_read_timeout 1h;
   }
   ```

   The controller only answers requests addressed to itself, so the proxy
   has to present them that way.

Opening the controller's plain address on your Wi-Fi keeps working as
before, without the offline copy. Through the proxy, the controller sees
every phone as the same device, so five wrong passwords pause sign-in for
everyone who uses that address for 30 seconds. Open Brew by Weight Companion
tells the controller which device each request comes from, so wrong
passwords pause only the device that typed them.

To check how a phone or computer is reaching the Web UI, open
**Diagnostic → Companion → This screen**. It shows whether the page runs in a
browser tab (**Web**) or from the home screen (**PWA**), whether it came
straight from the controller (**Direct**), through
[Open Brew by Weight Companion](../features/companion.md) (with the
companion's name and how many screens it serves right now), or through
another **HTTPS proxy**, and whether the offline copy is **Active** on that
device.

## Forgot the password

Use the paddle recovery steps in [Emergency recovery](../EMERGENCY_RECOVERY.md#recover-wi-fi-ap-and-password).
They restore the factory password and turn the Web UI password off. Recipes,
calibration, and shot history are kept, but Wi-Fi has to be set up again. A
factory reset also turns it off.
