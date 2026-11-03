# Home Assistant

Open Brew by Weight has a native Home Assistant integration. It shows each shot
on your dashboard a moment after it ends, lets you switch presets and flip the
header's **How shots stop** toggles from Home Assistant, and gives you a safe
restart button. It runs entirely on your local network and never starts or
stops the espresso machine.

The integration lives in its own repository:
**[Cheerpipe/ha-open-brew-by-weight](https://github.com/Cheerpipe/ha-open-brew-by-weight)**.
Its guides cover installing from HACS, every entity, automation and dashboard
examples, and troubleshooting from the Home Assistant side. This page covers
what you need on the controller.

## Get started

1. Install **Open Brew by Weight** from HACS and restart Home Assistant. The
   [integration's README](https://github.com/Cheerpipe/ha-open-brew-by-weight#install)
   has a one-click button.
2. In Home Assistant, set **Settings → System → Network → Local network** to an
   `http://` address on your LAN. The controller sends its updates there and
   can't use `https://`.
3. Look under **Discovered** in **Settings → Devices & services** and add the
   controller. If it isn't there, add the integration by hand and enter the
   controller's IP address or `<device-name>.local`, for example
   `openbrewbyweight.local` (see [Discovery by name](../settings/wifi.md#discovery-by-name)).

Home Assistant names the device after the controller's **Device name** under
Admin → Network, with each word capitalized; by default **Openbrewbyweight**.
Entity IDs are built from that name.

## What the controller needs

- Firmware with integration API version 1 and the `webhook_v1`,
  `preset_select_v1`, `quick_settings_v1`, `restart_v1`, and `stored_shots_v1`
  capabilities. Current firmware has all of them; Home Assistant refuses older
  firmware with a clear message during setup.
- The same trusted network as Home Assistant. The controller's API is open on
  the LAN, like its Web UI, so keep it off guest or shared networks. Discovery
  also needs the network to pass multicast.
- If the [Web UI password](../settings/web-access.md) is on, Home Assistant
  asks once for the device password, or shows a code to approve in the Web UI.
  It keeps only its own key, not the password. Turning the password on later
  makes Home Assistant ask for reauthentication; nothing is lost meanwhile.
  This needs an integration version that supports it, so update the
  integration first.

## Things to know on the controller side

- **One webhook receiver.** The controller sends updates to a single address.
  Adding the integration takes over the [generic webhook](webhooks.md) slot
  after asking you first; deleting the integration clears it only if Home
  Assistant still owns it.
- **Quick toggles are temporary everywhere.** A change from Home Assistant is
  the same as one from the header panel: it lasts until you pick another preset
  or the controller restarts, and nothing is saved to presets or Settings. A
  shorter **Duration** chosen in the panel, such as **Next shot**, also ends
  changes made from Home Assistant.
- **Nothing changes mid-shot.** Preset and toggle changes are refused during an
  active cycle. The restart button may be pressed during a shot, but the
  controller waits until it finishes, and it never resumes a cycle after boot.
- **The newest recorded shot is the one Home Assistant shows.** It's the same
  record as the idle Home card. Short, weightless, or 2 g-or-less activations
  don't replace it; deleting the newest history row reveals the next one, and
  clearing the log clears both views.
- **Update the integration first.** When a firmware release adds a new kind of
  value, such as a new stop reason, update the integration before the
  firmware. An older integration can't label values it hasn't seen.

## Check delivery from the controller

When Home Assistant isn't receiving updates, the controller can tell you why.
Open `http://<controller-address>/api/v1/integration/webhook` in a browser: the
`delivery` section shows whether updates are being sent, held back, or failing,
and the reason. While the Web UI password is on, that page needs Home
Assistant's key, so check the Web UI log under **Diagnostic → Log** instead. A failed update is retried after 3, 6, and 9 seconds.

To keep a record of failed attempts, set **Kept in memory** to **Warning**
under **Diagnostic → Log → Logging settings** in the Web UI; **Error** keeps only updates that
were finally dropped.

For the Home Assistant side, see the integration's
[troubleshooting guide](https://github.com/Cheerpipe/ha-open-brew-by-weight/blob/main/docs/troubleshooting.md).
The wire protocol both sides share is in the [Integration API](../INTEGRATION_API.md).
