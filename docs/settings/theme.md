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

## Hero card

The big card at the top of Home follows a shot from its first drop to its
final weight, and it comes in two looks. Pick the one you want to see:

- **Technical** sits on the same quiet surface as the rest of the
  interface and puts the recipe's graph right under the numbers. The graph
  shows the corridor your recipe allows — how fast is too fast, the green
  on-recipe window, how slow is too slow — with your shot drawn over it. While
  the shot runs, the zone the controller expects lights up and grows stronger
  as its forecast firms up, and a dotted arc points at the weight and moment
  it expects to stop at. The number of seconds sits in a chip that takes the
  color of that expectation, so you can read the pace from across the counter.
  After the shot, the zone it actually landed in stays lit, and you can drag
  the dot along the curve to read the time, the weight and the stop line at
  any point.
- **Colorful** (default) fills the whole card with the shot's color, and
  shows a simpler line drawing of the weight and flow. See
  [Shot card colors](#shot-card-colors) for how its color behaves while a
  shot runs.

The machine's default is chosen in **Admin → Frontend**, and every browser
and installed app follows it. An [installed app](app.md) can pick its own in
**Settings → This app**, for that device only — handy when the phone on the
counter wants the graph while a laptop elsewhere keeps the colorful card.
Double-tapping the card still opens the full-screen live view in both looks.

## Shot card colors

**Settings → This app → Shot card colors** — in the
[installed app](app.md) — chooses how the Home shot card uses
color while a shot runs. It shapes the **Colorful**
[hero card](#hero-card); the Technical card always follows the forecast on
its graph zones. So the setting is only available while the Colorful card is
the one in use: when Technical is chosen — directly or through the machine's
default — it switches off. Every finished shot takes the color of its result: green on recipe,
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
