# Web UI screenshots

These captures illustrate the layout in dark and light modes. They are historical examples,
not a specification of current labels, defaults, build options or permissions.
The current navigation uses a rounded header bar with icons and names, switches
to names alone when space is limited, and uses a bottom icon bar on phone-width
screens (up to 699 pixels wide), even when fewer menu items are visible. Wider
screens also use the bottom bar if the names cannot fit; the captures below
predate this layout.
Home's old No-scale toggle has been replaced by the
[mode-based policy](settings/no-scale-bbw.md). Use [first setup](GETTING_STARTED.md)
and the settings guides for current actions.

## Dark mode

### Home

Shot hero, presets and connection status. The captures predate the current
Home, which shows the shot hero, presets and equipment; how a shot stops
comes from the chosen preset, and the protections live in Settings. Remote
actions depend
on build policy and Admin unlock: the Actions panel is shown only when remote
machine control is compiled in and Admin is unlocked.

![Historical Home screen, taken before the shot hero replaced the last-shot panels](images/screenshot-home-dark.jpeg)

### Settings

The current page has two sections: **Presets**, saved with the selected
preset, and **Shared settings**, which apply to every preset. The captures
predate this layout and the old Brew / Machine and scale names. The physical
switch group depends on the compiled paddle or momentary machine type.

![Historical dark Settings screen](images/screenshot-settings-dark.jpeg)

### Statistics

See [history](features/shot-history.md) for current eligibility, sorting,
ratings and export behavior.

![Historical dark Statistics screen](images/screenshot-stats-dark.jpeg)

### Diagnostics

Technical status for problem reports. A single indicator does not establish
safe actuation. Loop gap is the largest gap between loop starts in the last
completed five-second window; its value and table breakdown update together
when the next measurement is available. Loop max is the largest since its last
**Reset**. The table breaks down the phases from each of those two events, even when the CPU
profiler is stopped. Delay call measures the pause until the task resumes,
including scheduling; loop dispatch measures the time from that return to the
next loop start. Other timing covers the remaining work outside the listed
phases. The figures may differ slightly from the rounded gap above. Select
**Reset** beside Loop max to begin a new maximum
measurement. In Date and time, **Last sync** shows the local date and time of
the most recent successful NTP sync, including while the device is retrying.
The current interface also shows the saved time zone and its applied UTC offset
on separate rows. This historical screenshot predates those rows.

![Historical dark Diagnostics screen](images/screenshot-diagnostic-dark.jpeg)

### Admin

Administration panel for Web UI access and the device password, Wi-Fi, OTA,
and factory reset.

![Historical dark Admin screen](images/screenshot-admin-dark.jpeg)

## Light mode

The same documented controls apply in light mode.

### Home

![Historical light Home screen](images/screenshot-home-light.jpeg)

### Settings

![Historical light Settings screen](images/screenshot-settings-light.jpeg)

### Statistics

![Historical light Statistics screen](images/screenshot-stats-light.jpeg)

### Diagnostics

![Historical light Diagnostics screen](images/screenshot-diagnostic-light.jpeg)

### Admin

![Historical light Admin screen](images/screenshot-admin-light.jpeg)

## Project banners

Use these images when you introduce Open Brew by Weight in a forum post, an
article or a social media profile. Each one comes in dark and light versions,
built from real captures of the Web UI.

| Use | Dark | Light |
| --- | --- | --- |
| Posts and link previews (2:1) | [banner-dark.jpg](images/banner-dark.jpg) | [banner-light.jpg](images/banner-light.jpg) |
| Wide page headers (16:5) | [banner-hero-dark.jpg](images/banner-hero-dark.jpg) | [banner-hero-light.jpg](images/banner-hero-light.jpg) |
