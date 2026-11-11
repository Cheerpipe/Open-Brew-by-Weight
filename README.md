<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/banner-hero-dark.jpg">
    <img src="docs/images/banner-hero-light.jpg" alt="Open Brew by Weight: brew by weight that just works. Free, open-source firmware for the ESP32-S3 with a predictive, adaptive cut, a real-time shot monitor, presets, shot history, statistics, three paddle modes, Home Assistant and La Marzocco Cloud integration. Works with Bookoo, Acaia, Felicita, Decent, DiFluid, Varia, Eureka and more.">
  </picture>
</p>

# Open Brew by Weight

Open Brew by Weight is free, open-source firmware that adds brew by weight to
the espresso machine you already have. A small ESP32-S3 controller sits inside
the machine, follows a Bluetooth scale, and stops each shot at your recipe's
weight, while you keep starting and stopping shots with the machine's own
paddle or button.

It is a do-it-yourself project: you buy a few inexpensive parts, install the
firmware, and wire the controller into the machine. It was developed on a La
Marzocco Linea Micra, which has the most complete guide; other machines with a
paddle or a push button can follow the same path with their own wiring.

> [!WARNING]
> **Safety and responsibility.** Installing Open Brew by Weight means opening
> your espresso machine and working on its electrical wiring, next to mains
> voltage, a pressurized boiler, hot water, and steam. Opening or modifying the
> machine may void its warranty. Open Brew by Weight is not a certified product:
> you install and use it at your own risk and under your sole responsibility,
> and the author and contributors accept no liability for any injury, damage,
> or loss. Always unplug the machine before you open it, and if you are not
> comfortable working on mains-powered equipment, have a qualified technician
> do the installation. Read [Hardware](docs/HARDWARE.md) before you open your
> machine and the full [Disclaimer](DISCLAIMER.md) before you install it.

## Why Open Brew by Weight

I wanted a [Shot Stopper](https://github.com/tatemazer/AcaiaArduinoBLE) by Tate
Mazer. It is a wonderful project, but its hardware layout limits how much of
the machine it can control, and its firmware covers the basics. What I was
after was brew by weight built into the machine as natively as possible, with
full control of the paddle and no workarounds: start a shot, and the automatic
stop simply works, with no conditions and no compromises.

That is how this firmware started. Without planning it, it grew into a serious,
advanced piece of software, packed with features the community asked for and
others designed around my own workflow, such as cut curves that move the stop
for fast and slow shots.

Six ideas shape every decision:

- **The machine's own interface.** The paddle remains the way you use the
  machine. Nothing to add, nothing new to learn.
- **The stock workflow.** Move the paddle and that's it, just like the stock
  machine or a Linea Mini with built-in brew by weight. It works without you
  noticing that the firmware is doing its job.
- **Complete, yet forgettable.** As complete as firmware can be, and robust
  enough to use every day and forget it is there.
- **Affordable.** The same result for about US$20–30 in parts, plus your scale,
  instead of US$200–300.
- **Easy to get, easy to build.** Starting this project isn't for everyone, but
  I have worked hard to make it as accessible as possible: the hardware is easy
  to find, and the project's tools make building, customizing, and installing
  the firmware as simple as they can be.
- **Open forever, integrated where it counts.** Free and open source for good
  and built to work with virtually any machine, while integrating as deeply as
  possible with the Linea Micra.

## Quickstart

**New here? Start with the [Quickstart](docs/QUICKSTART.md).** It takes the
reference build, an ESP32-S3 Relay X1 controller on a La Marzocco Linea Micra,
from a shopping list to your first shot by weight in six steps:

1. [Get the parts](docs/QUICKSTART.md#1-get-the-parts)
2. [Build and install the firmware](docs/QUICKSTART.md#2-build-and-install-the-firmware)
3. [Print the enclosure](docs/QUICKSTART.md#3-print-the-enclosure)
4. [Wire it into the Micra](docs/QUICKSTART.md#4-wire-it-into-the-micra)
5. [Connect it to your Wi-Fi](docs/QUICKSTART.md#5-connect-it-to-your-wi-fi)
6. [Pull the first shot](docs/QUICKSTART.md#6-pull-the-first-shot)

## Main features

| Need | Feature |
| --- | --- |
| Get started in minutes | Easy onboarding: a [guided setup page](docs/QUICKSTART.md#5-connect-it-to-your-wi-fi) on your phone walks you through how you brew, your password, and Wi-Fi |
| Stop at your recipe's weight | [Brew by weight](docs/features/brew-by-weight.md): a predictive cut with learned drip compensation that learns your machine shot by shot |
| Handle fast and slow shots | [Fast and slow shots](docs/features/fast-and-slow-shots.md): cut curves move the stop when a shot runs fast or slow |
| Use different recipes | [Presets](docs/features/presets.md): factory Single and Double in Adaptive and Fixed versions, plus your own |
| Follow the shot as it happens | A real-time shot timer and a live shot view while you brew |
| See everything at a glance | A live dashboard on Home: the shot chart with its cut line, your presets, and the state of your scale and machine |
| Review results | [Shot history and statistics](docs/features/shot-history.md), with export to Visualizer.coffee and the Gaggiuino format |
| Log shots in Beanconqueror | [Beanconqueror](docs/settings/beanconqueror.md) imports your shots in the Gaggiuino or GaggiMate format, as if the controller were one of those machines |
| Place the cup after starting | [Tare and retare](docs/features/tare-retare.md) |
| Handle cup removal or bumps | [Cup protection](docs/features/cup-protection.md) |
| Limit a shot after scale loss | [Scale-loss backup](docs/features/auto-to-manual.md): a timed limit takes over if the scale drops out |
| Rinse with a switch gesture | [Quick rinse](docs/settings/quick-rinse.md), with a machine-profile default |
| Use the scale you have | Bookoo, Acaia, Felicita, Decent, DiFluid, Varia, Eureka, and more; see [scale compatibility](libraries/EspressoScaleBLE/README.md#scale-compatibility) |
| Fit your machine | Paddle, momentary, and momentary-plus-reed builds; see [Machine types](docs/HARDWARE.md#machine-types) |
| Run on a small, inexpensive board | Extremely efficient use of the ESP32-S3: careful memory use and as few Wi-Fi transmissions as possible keep the radio free for your scale |
| Integrate with a Linea Micra | [La Marzocco Cloud](docs/settings/linea-micra.md) keeps your scale and the machine's power in sync, so turning the scale on can wake the Micra and turning it off can put it on standby; it also sets each preset's brew-boiler temperature and knows when the paddle is only waking the machine |
| Hear local feedback | [Alerts](docs/alerts.md): buzzer tones, or your Bookoo's own sounds |
| Connect your smart home | [Home Assistant](docs/features/home-assistant.md) (integration in [its own repository](https://github.com/Cheerpipe/ha-open-brew-by-weight)), [webhooks](docs/features/webhooks.md), and [notifications](docs/features/notifications.md) |
| Go further (advanced users) | The optional [companion](docs/features/companion.md), a small server at home that opens the Web UI over HTTPS, lets several screens use it at once, and takes La Marzocco's cloud work off the controller |
| Update safely without a USB cable | [Over-the-air updates](docs/features/ota.md) over Wi-Fi into a spare firmware slot, with boot verification and automatic rollback |
| Recover from a mistake | [Emergency recovery](docs/EMERGENCY_RECOVERY.md) restores access or resets settings from the machine's own switch |

## Documentation

| I want to… | Read |
| --- | --- |
| Go from parts to the first shot on a Linea Micra | [Quickstart](docs/QUICKSTART.md) |
| Check whether my equipment fits | [Requirements](docs/HARDWARE.md#requirements) and [machine types](docs/HARDWARE.md#machine-types) |
| Choose a controller and machine build | [Hardware and machine build profiles](docs/BUILD_PROFILES.md) |
| Build and install the firmware | [Build and USB installation](docs/BUILD.md) |
| Set up the controller and brew | [First setup and daily use](docs/GETTING_STARTED.md) |
| Change how brewing works | [Features](docs/README.md#features) and [settings](docs/README.md#settings) |
| Connect Home Assistant | [Home Assistant setup](docs/features/home-assistant.md) |
| Understand an unexpected result | [Troubleshooting](docs/FAQ.md) |
| Update or recover the controller | [OTA](docs/features/ota.md) / [Recovery](docs/EMERGENCY_RECOVERY.md) |
| Develop or contribute | [Contributing](CONTRIBUTING.md) and [testing and validation](docs/TESTING.md) |
| Look up a technical term | [Technical glossary](docs/GLOSSARY.md) |
| Find anything else | [Documentation index](docs/README.md) |

## License

Open Brew by Weight is licensed under the
[GNU Affero General Public License v3.0](LICENSE) (AGPL-3.0).

You may use, modify, and distribute it freely, including for commercial
purposes. If you distribute a modified version, or offer one to users over a
network, you must provide the complete corresponding source code under the
AGPL. Closed proprietary forks are not allowed.

Earlier snapshots of this repository were published under the MIT License.
Those historical releases remain available to their recipients under MIT.
The current code is an independent implementation: scale protocols were written
from official vendor documentation and public references, and obsolete
references to earlier projects have been cleaned up. See [LICENSE](LICENSE) for
the full terms.

### Logo and icons

The Open Brew by Weight logo and app icons are licensed separately under
[Creative Commons Attribution 4.0](LICENSES/CC-BY-4.0.txt) (CC BY 4.0). You can
use them in articles, videos, store listings, or your own projects, change
them, and use them commercially, as long as you give credit, for example:
"Open Brew by Weight logo by Felipe Urzúa, CC BY 4.0". Mention it if you
changed the artwork.

This applies only to the logo (`docs/images/OpenBrewByWeight.svg`), the brand
mark (`resources/icons/brand-mark.svg`), the favicon, and the app icons in
`resources/icons/android/`, `ios/`, and `windows/`, as listed in
[REUSE.toml](REUSE.toml). Everything else stays under the AGPL-3.0.

The license covers the artwork, not the name. Please don't use the name or
logo in a way that suggests your product or fork is the official Open Brew by
Weight project.

## Credits

Open Brew by Weight is maintained by **Felipe Urzúa**
(`cheerpipe@gmail.com`) —
[Cheerpipe/Open-Brew-by-Weight](https://github.com/Cheerpipe/Open-Brew-by-Weight).

Thanks to **[Tate Mazer](https://github.com/tatemazer)**, whose [Shot Stopper](https://github.com/tatemazer/AcaiaArduinoBLE) inspired this project.

Thanks to **[AtomHeart-Lang](https://github.com/AtomHeart-Lang)** for the
AtomHeart Eclair protocol information that keeps Eclair in the supported scale
set.

The EspressoScaleBLE library also credits:

- [LunarGateway](https://github.com/frowin/LunarGateway/) (frowin)
- [pyacaia](https://github.com/lucapinello/pyacaia) (lucapinello)
- Felicita Arc: baettigp, A-TWJ
- Bookoo: philgood, same31
- AtomHeart Eclair: [AtomHeart-Lang](https://github.com/AtomHeart-Lang)
- Decent, DiFluid, MyScale, Varia, Eureka, WeighMyBru: protocol knowledge from
  [gaggimate/esp-arduino-ble-scales](https://github.com/gaggimate/esp-arduino-ble-scales)
  ([jniebuhr](https://github.com/jniebuhr) et al.), reimplemented here without
  copying source
- Lunar 2019: jniebuhr

See the [library acknowledgement](libraries/EspressoScaleBLE/README.md#acknowledgement).

Runtime and tooling:

- [Espressif ESP-IDF](https://github.com/espressif/esp-idf)
- [Arduino-ESP32](https://github.com/espressif/arduino-esp32)
