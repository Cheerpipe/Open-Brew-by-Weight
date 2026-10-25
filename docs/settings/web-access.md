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
your network, with a **Set up** button that takes you to this panel. **Not now**
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
  Tap **Allow** only if the codes match. The request expires after 2 minutes.

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

## Forgot the password

Use the paddle recovery steps in [Emergency recovery](../EMERGENCY_RECOVERY.md#recover-wi-fi-ap-and-password).
They restore the factory password and turn the Web UI password off. Recipes,
calibration, and shot history are kept, but Wi-Fi has to be set up again. A
factory reset also turns it off.
