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

> Open Brew by Weight is not a certified product. Read [Hardware](docs/HARDWARE.md)
> before you open your machine, and the [Disclaimer](DISCLAIMER.md) before you
> install it. The relay stays open on startup, reset, power loss, and any fault.

## Why Open Brew by Weight

Weighing every shot is the simplest way to make espresso repeatable, but
stopping the machine at the right gram, shot after shot, is tedious, and few
machines do it for you. Open Brew by Weight brings that to the machine on your
counter instead of asking you to replace it.

A few ideas shape every decision in the project:

- **Your machine stays yours.** The paddle or button still starts every shot
  and can always stop it. The controller only adds the stop by weight, and
  remote start stays off unless you build it in on purpose.
- **Everything runs in your kitchen.** Brew by weight needs no cloud account,
  no subscription, and no Wi-Fi. Home Assistant, the companion, and the other
  integrations are optional extras.
- **It learns your machine.** Coffee keeps dripping after the machine stops,
  and every machine drips differently. The controller learns that drip shot by
  shot and stops early enough to land on your target.
- **Safety comes first.** The relay is the controller's only connection to the
  machine, and it opens whenever anything is wrong.
- **Open to everyone.** The firmware, the wiring guides, and the enclosure are
  free under the AGPL. The project was inspired by Tate Mazer's
  [Shot Stopper](https://github.com/tatemazer/AcaiaArduinoBLE).

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

## Features

- **Stops at your weight, and learns.** A predictive cut with learned drip
  compensation, and Adaptive recipes that adjust to fast and slow shots.
  See [Brew by weight](docs/features/brew-by-weight.md).
- **Recipes ready to go.** Single and Double presets in Adaptive and Fixed
  versions, plus your own. See [Presets](docs/features/presets.md).
- **Watch every shot.** A real-time shot timer and a live shot view while you
  brew, then [shot history and statistics](docs/features/shot-history.md) with
  export to Visualizer.coffee and the Gaggiuino format.
- **Beanconqueror integration.** [Beanconqueror](docs/settings/beanconqueror.md)
  imports your shots in the Gaggiuino or GaggiMate format, as if the controller
  were one of those machines.
- **Protects the shot.** [Tare and retare](docs/features/tare-retare.md),
  [cup protection](docs/features/cup-protection.md),
  [fast and slow shots](docs/features/fast-and-slow-shots.md), and a
  [backup timer](docs/features/auto-to-manual.md) if the scale drops out.
- **Works with your scale.** Bookoo, Acaia, Felicita, Decent, DiFluid, Varia,
  Eureka, and more; see [scale compatibility](libraries/EspressoScaleBLE/README.md#scale-compatibility).
- **Fits paddle and push-button machines.** Paddle, momentary, and
  momentary-plus-reed builds; see [Machine types](docs/HARDWARE.md#machine-types).
- **Talks to your Linea Micra.** [La Marzocco Cloud](docs/settings/linea-micra.md)
  sets the brew-boiler temperature of each preset and knows when the paddle is
  only waking the machine.
- **Fits your smart home.** [Home Assistant](docs/features/home-assistant.md),
  [webhooks](docs/features/webhooks.md), [notifications](docs/features/notifications.md),
  and the optional [companion](docs/features/companion.md).
- **Alerts you can hear.** [Buzzer tones](docs/alerts.md), or your Bookoo's own
  sounds.
- **Safe updates.** [Over-the-air updates](docs/features/ota.md) with automatic
  rollback, and [emergency recovery](docs/EMERGENCY_RECOVERY.md) from the
  machine's own switch.

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
