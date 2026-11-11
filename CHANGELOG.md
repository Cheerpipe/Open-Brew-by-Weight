# Changelog

Notable changes for people who install and use Open Brew by Weight. Version
numbers follow `MAJOR.MINOR.PATCH`; the firmware reports the same number in the
Web UI footer.

## 2.0.0 — 2026-11-11

Install this version over USB with a full erase: earlier shot curves and
exported JSON formats are not carried over.

- **Quickstart:** a new [Quickstart](docs/QUICKSTART.md) takes the Linea Micra
  build from a shopping list with reference prices to the first shot, with
  every setup screen shown. The README now introduces the project, its
  features, and where to start.
- **Import shots in Beanconqueror:** **Settings → Coffee apps → Beanconqueror**
  lets the app import your shots as if the controller were a Gaggiuino
  (Beanconqueror 8.2 or later) or a GaggiMate (8.8 or later, with your dose
  and rating). The section shows the address to paste, the steps, and a
  preview of your last shot as the app will receive it. The address has no
  password, like those machines'. A companion can serve it too, from its own
  copy and even with the machine off: **Admin → Companion → Beanconqueror
  through the companion**. See [Beanconqueror](docs/settings/beanconqueror.md).
- **Your Bookoo's own sounds on the buzzer:** when a Bookoo Themis connects,
  the buzzer plays the scale's own power-on melody, and when it disconnects, its
  power-off melody. Two new switches under **Alerts** turn this off; other
  scales keep the controller's tones. **Scale lost** is now called **Scale
  disconnected**. See [Alerts](docs/alerts.md#the-scales-own-sounds).
- **The companion and the controller's own address work side by side:** opening the
  controller's own address no longer interrupts the screens using the companion, or
  the reverse. The window you pair from keeps working, and Admin links to the Web UI
  through the companion. Opening Admin from the companion's page goes straight to
  its Companion section.
- **A companion opens the Web UI only once paired:** a companion that requires it
  takes you to its own page, with the code and a link to Admin, and a window left
  open goes there too. See [the companion's guide](docs/features/companion.md).
- **Pairing a companion no longer asks for the Admin password again:** if you
  were already using the Web UI through the companion, finishing the pairing used
  to lock Admin without warning. Now the page stays where it is and shows the
  result.
- **Companion sign-in hardened:** a device on your network could hold back your
  sign-in answer and collect it for itself. Answers are now tied to the request
  that asked, and the controller publishes a list of its Web UI files so a
  companion can check them. **Diagnostic → This screen** shows whether they were
  verified. This needs the companion's matching update; both keep working if
  you update them one at a time.
- **Strange names can't break the Web UI:** a scale, a companion or a stray web
  address with a name that isn't valid text used to cut the live connection, on
  Home, Shots or the log, over and over. Now it just shows a replacement
  character.
- **Firmware updates can't be blocked by a slow connection:** a device sending a
  refused update very slowly can no longer freeze the Web UI.
- **Time limits stay on if a setting is damaged:** if the controller can't read
  whether the firmware is enabled, it stays enabled, with its relay limits.
- **Finished shot numbers read whole:** in the live view, the card of a finished
  shot no longer cuts off the corners of its first letters and digits.
- **Notifications:** an app installed from the companion's address can tell you
  when a shot ends, with its time, yield, and result, even with the app closed.
  Tap one to open that shot's card. Turn it on in **Settings → This device**;
  see [Notifications](docs/features/notifications.md). On a Linea Micra, it can
  also tell you when La Marzocco needs you to accept new terms; tap it to open
  Home and see what to do.
- **Live view when you rotate the phone:** turning the phone with the live view
  open no longer leaves the chart squeezed into the middle of its card, on an
  iPhone in particular. The chart now fills the card again at once.
- **Live view on short phones:** with a phone on its side, the cards at the
  bottom of the numbers column no longer run off the screen during a shot. To
  make room, the weight shrinks a little on the shortest screens and **Max flow**
  leaves out when it happened; held upright, it still shows it.
- **Keep the live view in landscape:** a new choice in **Settings → This
  device**, for the installed app, lays the live view out sideways even with the
  phone upright or its rotation locked. See
  [Settings for this device](docs/settings/app.md).
- **Grind hints no longer stuck on old advice:** a suggestion from your first
  shots could silently keep a card in the opposite direction from appearing,
  even many shots later. Once the shots being read are all newer than that
  suggestion and its card is gone, it no longer holds new advice back.
- **Every shot is kept:** each shot pulled with a scale is stored, even one
  that ran short or never reached a usable yield. Those are marked as bad
  shots: they never count in statistics, charts, grind hints or learning,
  and **Show bad shots** in the Shots filter lists them with their reason.
  Only good shots are numbered, so a quick purge never makes the count jump:
  a bad shot's card shows a grey **Bad** tag instead of a number, and the
  shots you already have are renumbered once when you update.
- **Shot trace:** each shot also records what the controller saw and
  decided, from the tare to the last drip. Open it from a shot's menu with
  **View trace**, or export everything with **JSON + diagnostics** (also as
  a compressed `.json.gz`). A shot cut short by a crash or restart is kept
  as an interrupted trace.
- **More history:** up to 1,000 shots, with curves kept on flash and the
  most recent ones always at hand. Diagnostic gains a **Shot storage**
  section that shows how the space is used.
- **Late tare fix:** when the scale applies the shot-start tare late, the
  controller now recognizes it, so the shot still stops at its target
  instead of running to the time limit; a weight that drops below an empty
  scale hands over to the timed backup.
- **Cup not tared:** Home tells you when the cup on the scale was not tared
  so you can lift it and place it again, and the Diagnostic log says why.
- **Live forecast curve:** in the Live view, the dashed projection toward the
  cut line now follows the shot's own pace. It bends upward while the flow
  speeds up and its ring marks where the controller expects the shot to land,
  so the estimate no longer reads late on shots that accelerate as they go.
- **Steadier landing point:** the point where the controller expects the shot
  to land, on Home and in the Live view, now appears once the flow has
  settled and glides toward its estimate instead of jumping between the fast,
  on-recipe and slow zones early in the shot.
- **Continuous curve hardness:** the transition curve of Fast and Slow shots
  is no longer three fixed levels. A slider from **Gentle** to **Firm** sets
  how marked the curve is, and every graph draws the exact cut line you
  chose. Presets saved with the old Soft, Medium or Hard levels brew exactly
  as before.
- **JSON export:** one format, version 1, replaces the formats shipped up to
  1.1.0.
- **Deleting needs Admin:** deleting a shot or activation, and clearing a
  whole history, now need Admin unlocked; **Clear** moved from Stats and
  History to Admin. With a companion, a deleted record or a cleared history
  is gone from its copy too, however you open the Web UI, and its own page no
  longer deletes anything.
- **Log history through the companion:** Diagnostic loads older events as
  you scroll, applies its filters to the whole history, jumps to a date and
  time, exports a date range, and deletes the companion's log history while
  Admin is unlocked.
- **Stats through the companion:** when the companion is still catching up,
  Stats say they show only the controller's shots, instead of switching
  silently.
- **Shots and History from the companion's copy:** with **Shot archive in
  the companion** on, the companion answers Shots and History itself, one
  list over your whole history, ready before you open them, so scrolling,
  filtering, exporting and shot charts ask the controller for nothing. A new
  shot reaches the companion right after its drip delay, and during a shot
  the controller and the companion do not talk at all. Admin → Companion
  shows how up to date the copy is. Update the controller and the companion
  together: until both are updated their link waits, while the Web UI (and
  firmware updates) still open through the companion.
- **Quiet La Marzocco connection during scaled shots:** when the controller
  follows the Linea Micra itself over the live connection, a shot with a
  connected scale pauses La Marzocco updates from its start until the pump
  stops, so the scale has the radio to itself. The connection stays open and
  updates resume the moment the pump stops.
- **A new Home card:** the card at the top of Home shows your last three
  shots, and a new **Technical** look draws the recipe's graph under the
  numbers, with your shot over it. Pick it in
  [Theme](docs/settings/theme.md#hero-card).
- **Scale battery:** a Bookoo Themis, Felicita Arc or recent Acaia shows its
  charge in the header, the live view, Home and Diagnostic, and warns you as
  it runs low. See [Scales](docs/settings/scales.md#scale-battery).
- **Lock the presets on Home:** the preset list starts locked, so a stray tap
  no longer switches your recipe. Press and hold a preset to switch, or the
  lock to unlock. See [Presets](docs/features/presets.md#lock-the-preset-list-on-home).
- **Update and Wi-Fi results on Home:** after a firmware update, Home waits
  for the restart and tells you how it went, in every open browser; a Wi-Fi
  change reports its outcome the same way.
- **A redesigned Diagnostic log:** step through the log by day and by boot,
  jump back to the newest line, and read far more history at once.
- **Learning kept across restarts:** what the controller learned about your
  drips survives a restart, and a drip hold cut short keeps the weight
  it had already settled at.
- **The Linea Micra's own temperature range:** the brew temperature in your
  presets accepts the range the machine reports, and nothing is sent until
  the machine has reported it.
- **Two power settings instead of one:** **Performance boost** and **Idle
  power saving** replace the single power management switch in Admin; your
  previous choice carries over to both. See
  [Power management](docs/settings/power-management.md).

## 1.1.0 — 2026-10-26

The first published stable release; it supersedes the unpublished 1.0.0 tag.

- **Brew by weight:** a Bluetooth scale measures the drink and the controller
  stops the shot near your recipe's target weight, learning from each shot.
  You keep using the machine's own brew switch.
- **Presets and shot monitor:** saved recipes, a live shot view, shot history
  with curves, statistics, and an activation history. Each stored shot keeps
  the machine's target temperature (when the integration reports one), the
  preset's dose, and the resulting brew ratio, shown on its history card and
  included in the CSV export. Preset graphs and the shot card on Home name
  how far the stop line moves for fast and slow shots, for example
  **Up to 42 g** and **Down to 33 g**.
- **Shot export:** save a shot as JSON or in the Gaggiuino format, open it in
  Beanconqueror, or export it for Visualizer.coffee with its recipe details.
- **Safety backups:** cup protection, a timed fallback when the scale drops
  out, and a relay that always stays open on startup, reset, and power loss.
- **Machines and switches:** paddle, momentary, and momentary-plus-reed
  builds, plus the La Marzocco Linea Micra cloud integration.
- **Integrations:** Home Assistant and generic webhooks. Starting a shot
  remotely stays off unless the firmware is built with that option.
- **Updates over Wi-Fi:** each image is checked against your controller and
  machine before it installs, and the previous version stays available for
  rollback.
- **Web UI password:** optional sign-in for the Web UI, remembered devices,
  and protection against look-alike web addresses. First setup asks you to
  replace the public factory password with your own.
- **Diagnostic log:** Bluetooth radio messages have their own **ble**
  category, and everything about your scale (finding it, connecting, its
  readings and commands) appears under **scale**, so you can follow a scale
  problem with the category filter instead of searching the **system** lines.
- **Scheduled restart:** an optional daily restart at a local time you choose
  in Admin. It waits for shots and firmware updates, skips the day rather
  than interrupting you, and records each restart in the audit log.

**Installing 1.0.0 over an earlier development build** needs one USB
installation with `--erase-all`: version 1.0.0 uses a new storage layout, so
settings, Wi-Fi, the password, and history start fresh. Later 1.x updates work
over Wi-Fi as usual. See [clean installation](docs/BUILD.md#clean-installation).
