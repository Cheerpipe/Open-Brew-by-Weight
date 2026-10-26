# Changelog

Notable changes for people who install and use Open Brew by Weight. Version
numbers follow `MAJOR.MINOR.PATCH`; the firmware reports the same number in the
Web UI footer.

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
