# Theme

The interface offers three color themes. To switch between them, tap the
**theme button** in the header, next to the Wi-Fi and Bluetooth signal icons.
Each tap moves to the next mode: automatic, light, dark, and back to
automatic. The icon shows the active mode: an empty circle for light, a half
filled circle for automatic, and a full circle for dark.

- **Auto** (default) follows the light or dark preference of the phone or
  computer you are browsing from, and switches on its own when the system
  changes.
- **Light** and **Dark** fix the interface to that palette whatever the
  system does.

The choice is saved per browser on the device you browse from, so a phone
and a laptop can use different themes with the same controller. It lives in
the browser's local storage rather than the controller's saved
configuration: clearing the site's browser data returns the interface to
Auto, and a [factory reset](factory-reset.md) does not change it.

## Shot card colors

**Admin → Frontend → Shot card colors** chooses how the Home shot card uses
color. Every finished shot takes the color of its result: green on recipe,
red when fast, blue when slow. The options differ in what you see while a shot
is running:

- **By forecast** (default) fades the card from coffee brown toward the color
  it expects for the result — red for fast, green for on recipe, blue for slow
  — while the shot runs. The color grows stronger as the shot progresses and
  as the controller's confidence grows, and it reaches its full shade a few
  seconds before the shot ends, so you can read the outcome at a glance before
  the machine stops. A slow shot also shows the weight it is heading for, and
  a stalled one tells you what it is tracking to, in case you want to stop it
  early.
- **By time window** instead follows the clock: green while the shot is inside
  its time window, blue once it passes it, red only when the controller is
  sure the shot is running fast.
- **Fixed color** keeps the usual brand color for every shot; the card still
  names the result in words.

Color changes fade in smoothly; with reduced motion turned on in your system
settings they switch at once. Like the theme, this choice is saved in this
browser only.
