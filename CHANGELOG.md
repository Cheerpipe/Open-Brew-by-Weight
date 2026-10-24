# Changelog

Notable changes for people who install and use Open Brew by Weight. Version
numbers follow `MAJOR.MINOR.PATCH`; the firmware reports the same number in the
Web UI footer.

## 1.0.0 — unreleased

The first stable release.

- **Brew by weight:** a Bluetooth scale measures the drink and the controller
  stops the shot near your recipe's target weight, learning from each shot.
  You keep using the machine's own brew switch.
- **Presets and shot monitor:** saved recipes, a live shot view, shot history
  with curves, statistics, and an activation history. Each stored shot keeps
  the machine's target temperature (when the integration reports one), the
  preset's dose, and the resulting brew ratio, shown on its history card and
  included in the CSV export.
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
  and protection against look-alike web addresses.

**Installing 1.0.0 over an earlier development build** needs one USB
installation with `--erase-all`: version 1.0.0 uses a new storage layout, so
settings, Wi-Fi, the password, and history start fresh. Later 1.x updates work
over Wi-Fi as usual. See [clean installation](docs/BUILD.md#clean-installation).
