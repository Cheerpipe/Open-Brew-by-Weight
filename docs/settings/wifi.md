# Wi-Fi

Outbound cloud communication, webhook delivery and time synchronization pause
while a detected compatible scale is being acquired. Wi-Fi scan/reconnect work
follows the same acquisition gate. The local Web UI remains available; the
controller does not deliberately disconnect station Wi-Fi. Already-started
network setup can take a moment to finish before its owner closes the connection.

How the controller joins your home network (STA) and when it falls back to
its own access point. Details for the fallback AP itself are in [AP](ap.md).

The Web UI is reachable from any client on the same network. Home and
Settings use a single-browser claim (`Reload`); see
[Web access](../GETTING_STARTED.md#web-access). **Admin** is additionally locked
behind the device password. Unlock stays active while that Admin page is
open, or for 15 minutes after the last privileged action; **Lock** closes
it immediately. Use a trusted network.

## When it applies

Wi-Fi and the HTTP server start regardless of paddle position.
**Configuration changes** still need a maintenance window: paddle OFF, machine circuit
open, no active cycle.

You cannot change workflow settings during an active shot. Read-only status
stays available.

## Parameters and behavior

| Setting / behavior | Default | Effect |
| --- | --- | --- |
| **Home Wi-Fi (STA)** | none on a fresh flash | Saved SSID/password. Device joins your network and serves the Web UI at the STA IP. |
| **Device name** (Admin → Network) | `openbrewbyweight` | Friendly name for the controller on your local network. 1–32 letters, digits, spaces, or hyphens; no leading/trailing space or hyphen. Renaming saves immediately without restarting Wi-Fi, survives reboots and **Forget network**, and returns to the default only on factory reset. See [Discovery by name](#discovery-by-name). |
| **Wi-Fi sleep** (Admin → Power management) | on | When on, STA uses modem sleep (`MIN_MODEM`) whenever it is associated and SoftAP is down. Stays `NONE` on SoftAP, during OTA, while STA is disconnected, or when this toggle is off. Saving it with the Power management group's **Save settings** button needs no restart and no reconnect wait, and the toggle stays disabled until a network is configured; a full network save still carries the saved sleep value. It remains independent of the [ESP32 power management](power-management.md) setting, WebUI activity, and scale connection state. Factory default on. Scale discovery duty is Admin → Power management → BLE scan mode, not this toggle. |
| **IP mode** | DHCP | **DHCP** or **static** (`ip` / `netmask` / `gateway` / `dns1` / `dns2`). |
| **Confirm window** | 3 minutes | After a Web UI STA save, a wait overlay retries the current address for confirmation. If the network/IP changed, reconnect your client and open the new IP yourself. The first successful UI claim confirms the new network. If this page never returns, previous network settings are restored. USB `SET_WIFI` commits immediately. |
| **Boot with no credentials** | SoftAP up | SoftAP at boot with a **3 min** idle shutdown when no SoftAP stations are associated. See [AP](ap.md). |
| **Boot with credentials** | STA first | SoftAP only if STA does not associate in about **25 s** (and STA never joined this boot). Then AP+STA until STA connects or SoftAP idle-stops; SoftAP is then stopped. |
| **STA drops after a successful join** | retry STA only | SoftAP is **not** raised automatically. Use USB `AP_START` or reboot. |
| **Saved network unreachable** | retry every 10 s, slowing to every 2 min | Each retry searches every channel, so while your router stays down the controller waits twice as long after each failed attempt, up to 2 minutes, to leave airtime for the scale and save power. The interval returns to 10 s when you connect or save from the Web UI, when the link drops after working, or when a phone joins the setup network. Once the router is back, expect the controller to rejoin within that interval. |
| **Automatic time zone** (Admin → Date and time) | On for a fresh installation | Follows the phone or computer using the Web UI. Turn off to choose a zone manually, then **Save settings**. |
| **Time zone** (Admin → Date and time) | Detected from the browser | Saves a region/city such as `America/Santiago`. Read-only while automatic mode is on. The controller applies that zone's daylight saving rules to new local times. Until a zone is configured, it uses UTC. |
| **Automatic date & time** (Admin → Date and time) | On | Keeps the controller clock accurate by syncing with the NTP server below. Turn off to stop automatic syncing; **Sync now** still sets the clock once on demand. |
| **NTP server** | pool | Preset or custom hostname for time sync. |

On a fresh installation or after a factory reset, opening any Web UI page
starts time-zone setup when the controller is ready for configuration. You do
not need to enter or unlock **Admin**. This initial setup remains active until
a zone has actually been saved to the controller, even if automatic mode is
off. Detecting or applying a zone alone does not complete setup: failed writes
are retried, and setup resumes if the controller restarts before saving.

In **Admin → Date and time**, turn on **Automatic time zone** to select this
phone or computer's detected zone and lock the selector. Turn it off to enable
manual selection and stop automatic detection after initial setup. Check the
preview, then **Save settings** to keep the mode and zone across restarts.
If detection is unavailable, turn automatic mode off and choose a supported
zone manually. The preview uses your browser's clock until NTP has set the
controller's clock and labels that estimate clearly.

While automatic mode is saved and the Web UI is active, its regular updates
check the browser's current zone on any page: normally every four seconds,
or every twenty seconds in Stats and History. Changes wait until configuration
is allowed. Opening the UI from another device can therefore change the zone.
Manual mode keeps your saved selection. The status below the selector shows
when the zone is saved or whether the controller is retrying a failed write.
Edits made during a previous save remain available for your next save.

NTP supplies the current UTC time; the saved zone determines how that instant
appears locally. Changing the zone does not change NTP, past history entries,
or the controller's brew timers. **Diagnostic → Date and time** shows the saved
zone and the offset currently applied to it as separate values. Daylight
saving changes update that offset automatically. The controller includes
IANA rules for 2025–2099; future legal changes require a firmware update.

First-time setup is guided: join the controller's [setup network](ap.md#first-connection)
and the setup page opens by itself (or at `http://192.168.4.1/setup`), asks for
your network and its password, and connects live — no restart. **Connect**
tests the network and **Save Wi-Fi** keeps it, so a wrong pick is never
saved. The table's **Home Wi-Fi (STA)**, **IP mode**, and **Confirm window**
rows describe the **Admin → Wi-Fi** page, which is the advanced path for
changing an already-configured network.

## Discovery by name

The controller announces itself on the local network using the **Device name**
(Admin → Network), so you can open the Web UI at
`<name>.local` — for example `openbrewbyweight.local` — instead of hunting
for the IP address, and the device shows up when you browse for web services
(`_http._tcp`). The address also works while the controller runs its own
access point (then it resolves to `192.168.4.1`).

Spaces become hyphens and letters are lowercased for the address, so
`Cafe Bar` is reachable as `cafe-bar.local`. Two controllers with the same
name on one network resolve the conflict automatically with a numbered
suffix; rename one to keep them apart. The same announcement also lets the
Home Assistant integration find the controller on its own, so it usually
shows up under **Discovered** without typing an address.

Discovery relies on mDNS, which most home routers, computers (macOS,
Windows 10+, Linux with Avahi), and Android 12+ phones support out of the
box. If your phone or browser cannot open `.local` addresses, use the IP
address shown in Admin → Network. The Web UI also works when opened through
another name for the controller, such as one your router assigns or a reverse
proxy on your network. Discovery keeps working with
[Wi-Fi sleep](power-management.md) enabled; first responses can take a
moment longer while the radio sleeps between the router's beacons.

When a service browser or Home Assistant lists the controller, it shows the
device name as you wrote it, with each word capitalized — `Cafe Bar` appears
as `Cafe Bar`, and the default name appears as `Openbrewbyweight`. Only the
`.local` address is lowercased with hyphens; the friendly display name keeps
its capitalization.

Scan lists up to **12** networks. The firmware scan operation has a **20 s**
timeout; the browser's overall wait can be longer. Cancel from
the same maintenance window.

## Example

From the setup page (see [AP](ap.md#first-connection)), connect to your home
network: the controller joins it live, and **Save Wi-Fi** keeps it. The page
then reminds you to switch your phone back to that network and shows the new
addresses, and the setup network stays open for about a minute (or until you
select **Finish**) before closing by itself. From **Admin → Wi-Fi** the classic save
still restarts the controller and waits for you to reopen the new address
within 3 minutes to confirm. If you later lose that network, the device keeps
retrying STA. Recover the setup network with USB
`AP_START` (see [USB serial CLI](../SERIAL_CLI.md)) or a reboot.

OTA over Wi-Fi: [OTA](../features/ota.md). Scripts:
[Build scripts](../SCRIPTS.md).

Related: [AP](ap.md), [Factory reset](factory-reset.md).
