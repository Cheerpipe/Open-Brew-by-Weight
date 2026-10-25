# AP

Fallback Wi-Fi access point used when no home network is saved, or when saved
STA fails to associate at boot.

## When it applies

- Fresh flash or [factory reset](factory-reset.md): SoftAP is up at boot.
- Saved STA fails to associate for about **25 s** at boot: SoftAP comes up
  and STA keeps retrying in AP+STA until STA connects. Then SoftAP stops —
  right away when nobody is on it, or about a minute later when a phone is
  still joined, so its setup page can show where to find the controller.
- SoftAP auto-raise is **boot/bootstrap only**. After a successful STA join,
  a later drop does **not** raise SoftAP. Use USB `AP_START` or reboot.
  See [USB serial CLI](../SERIAL_CLI.md).

## Idle shutdown

Auto SoftAP stays up for at most **3 minutes** with zero SoftAP stations.
If a phone or laptop joins the AP, SoftAP stays up while that client is
associated. When the client count returns to zero, a new 3-minute idle
countdown starts. After idle shutdown, SoftAP stays down for the rest of
the boot (USB `AP_START` or reboot to raise it again). Manual `AP_START`
keeps SoftAP up without the idle timer until `AP_STOP`, or until a new home
network is set up from the setup page (then it closes about a minute later).

With Open Brew by Weight switched on, turning the machine on (paddle ON or a
switch press) closes this automatic setup network right away, even with a phone
joined, so the scale search starts at once instead of waiting for the idle
timeout. This helps after a power cut when the router is still starting: just
turn the machine on to brew. The controller keeps trying your home network in
the background and joins it when it is back. A setup that is connecting to a
network, or waiting for you to save the network it just joined, keeps the
setup network up, and so does `AP_START`. With the firmware
switched off the paddle never closes it, so a new owner can still finish setup
after pulling a shot.

The AP name is unique to this controller: **`OpenBrewByWeightAP-`** plus
eight lowercase characters from the end of this device's AP address. USB
`AP_STATUS` prints the exact name. SoftAP WPA2 uses the **device password**.
The factory password is the same on every controller, so the first part of
the setup page always replaces it with yours (typed twice). The setup network
you are on keeps running with the old one until it next starts.
Change it later from **Admin** (unlock with the device password) **→ Access**
or from USB (`SET_DEVICE_PASSWORD` / `RESET_DEVICE_PASSWORD`). Admin unlock,
OTA, and the optional [Web UI sign-in](web-access.md) use that same device
password.

## Parameters

| Setting | Default | Notes |
| --- | --- | --- |
| **AP name** | `OpenBrewByWeightAP-xxxxxxxx` | Not user-editable. The eight characters are unique to this controller. USB `AP_STATUS` shows the live name. |
| **Device password** | `ineedacoffee` | Case-sensitive. 8–63 characters when you change it; USB `SET_DEVICE_PASSWORD` will not accept the factory string as the new value. SoftAP WPA2, Admin unlock, OTA, and the optional [Web UI sign-in](web-access.md) all use this same device password. |
| **AP address** | `http://192.168.4.1` | SoftAP IPv4. |

## First connection

1. Power the ESP32-S3 and wait for boot (the GPIO 1 LED stays off until a
   scale connects).
2. Join **`OpenBrewByWeightAP-xxxxxxxx`** (the unique name on this
   controller; USB `AP_STATUS` prints `ssid=`) with the device password
   **`ineedacoffee`**. Forget any saved network that is only
   `OpenBrewByWeightAP` with no suffix. The scale may wait to connect
   until you finish this setup network.
3. The setup page opens by itself. If nothing pops up, open
   **`http://192.168.4.1/setup`** in a browser. Joining this network already
   proves the device password, so the page never asks for the current one.
   It has three parts, with a progress bar at the top:
   - **Your controller.** On a new or factory-reset controller it asks how
     you want to brew: **Brew as usual** keeps the machine exactly as before
     (the [firmware](firmware-mode.md) stays off), and **Stop shots by
     weight** switches the firmware on and asks for a starting recipe
     (**Manual** is selected; each recipe shows its dose, yield and ratio).
     Then you choose your own password, typed twice; a checklist shows when
     it is long enough, differs from the factory one, and both entries
     match. **Save and continue** stores all of it at once, without
     restarting, and lists exactly what it saves.
   - **Wi-Fi.** Pick your home network, enter its password, and select
     **Connect**. The controller joins your network while this setup network
     stays up, and the page shows **Connected to** your network. Nothing is
     kept until **Save Wi-Fi**; **Wrong network? Choose another** tests a
     different one instead. A tested network you do not save is forgotten
     after ten minutes (the page counts down), and a network saved earlier
     stays in place. If your phone drops off the setup network while the
     controller joins (some phones follow the router's channel), join it
     again: the page reopens on the same step.
   - **Review and finish.** The summary marks everything that is saved.
     **Change** edits one item and saves it again; for Wi-Fi, **Keep** (with
     your network's name) leaves the saved one in place. The page reminds
     you to switch your phone back to your home network and lists the
     addresses to use from now on, each with a **Copy** button. The setup
     network closes by itself about a minute after the Wi-Fi is saved (the
     page counts down), or right away with **Finish**. Choosing to stop
     shots by weight restarts the controller once when you select **Finish
     and restart** (or when the setup network closes); turn on your scale
     afterwards.
   If the connection fails, the reason appears at the top of the card and
   your entries stay in place: correct them and select **Connect** again, or
   use **Search again** when your network is missing from the list.
4. Continue with [first setup](../GETTING_STARTED.md). Changing networks
   later (including a static IP address) is done from
   **Admin → Wi-Fi** on your home network.

The setup page always shows the step the controller is really in. If the
sign-in window hands over to your browser, you reload, or you open the page
on a second phone, it picks up where things are — still connecting,
connected and waiting for **Save Wi-Fi**, saved, or ready for another try —
and it is never locked by another
window or by inactivity. While the machine's paddle is on or a shot is
running, Wi-Fi can't be changed: the card says so and carries on by itself
once the machine is idle again.

While you are on this AP, every name resolves to the controller, and phones
and computers detect that sign-in is required — that is what makes the guided
setup page open on its own. The controller's
[device name](wifi.md#discovery-by-name) also resolves while you are here.

Not ready to configure Wi-Fi yet? Choose **Continue without Wi-Fi** on the
setup page. Your brewing choice and password are already saved, and
**Finish** (or **Finish and restart**) ends setup. The sign-in window closes on its own, but your phone stays
joined to the setup network and the setup page remains open at
`http://192.168.4.1/setup` until the setup network itself shuts down —
reopen it there whenever you want to continue.

If home Wi-Fi is lost but you know the device password, use the AP after
reboot / `AP_START`: it serves the same guided setup page, and the whole
flow is passwordless while you are on it. A forgotten password also prevents
joining the protected AP: use USB or
[physical recovery](../EMERGENCY_RECOVERY.md) to restore access.

Related: [Wi-Fi](wifi.md), [Factory reset](factory-reset.md).
