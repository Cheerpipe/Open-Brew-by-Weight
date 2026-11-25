# Build and install the firmware

<a id="build-environment"></a>

This guide takes you from a computer with nothing installed to an ESP32-S3
Relay X1 board running Open Brew by Weight. You compile the firmware yourself
on your computer, then copy it to the board over a USB cable. Later updates
can go over Wi-Fi.

Steps 1 to 8 are all most people need. They use the standard settings, and
every command can be copied and pasted as it is. Everything else, such as
custom options, development builds and the details behind each check, is in
[For advanced users](#for-advanced-users) at the end. You do not need it to
build and install the firmware.

> [!WARNING]
> **These step-by-step instructions are new and have not been tested on every
> system yet.** The firmware was developed and tested on macOS. The Linux
> commands in steps 1 to 6 were run once, exactly as written, on a fresh
> Ubuntu 24.04 installation (in a container), up to a successful build. Installing over USB on Linux, and everything on
> Windows, has not been tested yet. If a step fails for you, please
> [open an issue](https://github.com/Cheerpipe/Open-Brew-by-Weight/issues)
> with the command you ran and the message you got.

> [!NOTE]
> This guide covers the version of the firmware it comes with. Older releases
> may need different tool versions and are not covered here.

## The tools you will use

You install these once. All of them are free.

| Tool | What it does for you |
| --- | --- |
| **Git** | Downloads the project's source code and keeps track of its versions. |
| **Python** | Runs the project's build commands and the ESP-IDF tools. |
| **Node.js** | Prepares the web interface that the controller serves to your phone. |
| **ESP-IDF** | Espressif's official kit for ESP32 chips: the compiler that turns the source code into firmware, and `esptool`, which copies that firmware to the board. |
| **CMake and a C++ compiler** | Used by the project's self-checks on your computer. |
| **`./scripts/dev`** | The project's own command. You use it to check your setup, build the firmware, and install it. It ships with the project, so there is nothing extra to install. |

### Which computers work

Git, Python, Node.js and ESP-IDF all run on Windows, macOS and Linux. The
project's own build scripts, however, are written for Unix-style systems:

- **macOS:** works directly. This is where the firmware was developed and
  tested.
- **Linux:** works directly. These steps use Ubuntu or Debian; Ubuntu 24.04
  is the reference.
- **Windows 10 or 11:** works through **WSL2** (Windows Subsystem for Linux),
  Microsoft's free, official way to run Ubuntu inside Windows. Once it is set
  up, you follow the Linux instructions in an Ubuntu window. A small free
  tool, **usbipd-win**, lets that Ubuntu window reach the board's USB port.

## Before you start

You need:

- an **ESP32-S3 Relay X1** board (the N16R8 model, see [Hardware](HARDWARE.md));
- a **USB-C data cable**. Many cables only charge and carry no data; if your
  computer never sees the board, try another cable first;
- an internet connection and about **10 GB of free disk space**;
- about **an hour** the first time. Most of it is waiting for downloads.

Building the firmware does not need the board. You only connect it in
step 7. Keep it disconnected from the espresso machine for the whole guide.

### How to use the commands in this guide

Every grey box is a command for the **terminal**, the text window where you
type commands:

- **macOS:** open **Terminal** (press <kbd>Cmd</kbd>+<kbd>Space</kbd>, type
  `Terminal`, press <kbd>Enter</kbd>).
- **Linux:** open **Terminal** from your applications menu
  (<kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>T</kbd> on Ubuntu).
- **Windows:** step 1 tells you which window to use for each command.

Then:

1. Copy the whole box with the copy button in its top-right corner.
2. Paste it into the terminal: <kbd>Cmd</kbd>+<kbd>V</kbd> on macOS,
   <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd> in a Linux or Ubuntu
   terminal, or right-click in the window.
3. Press <kbd>Enter</kbd> and wait until the prompt (the line where you
   type) comes back before running the next box.

A few things that surprise people:

- A command that ends with a `\` continues on the next line. Copy the whole
  box: it is still one command.
- When a command asks for your password, **nothing appears while you type**,
  not even dots. That is normal. Type it and press <kbd>Enter</kbd>.
- `sudo` at the start of a command means "run this as administrator". It
  asks for the password you use to sign in to your computer (or, in WSL, the
  Ubuntu password you choose in step 1).
- Lines of text such as warnings that scroll past are usually fine. A step
  has failed only if it stops with a clear `error` message. When that happens,
  look for the message in [Troubleshooting](#troubleshooting).

## 1. Prepare your computer

<a id="2-install-host-prerequisites"></a>

Follow only the section for your system.

<a id="macos-maintainer"></a>

### macOS

**1. Install Homebrew**, the usual way to install developer tools on a Mac.
It also installs Apple's Command Line Tools, which include Git and a C++
compiler. It asks for your Mac password and may take 5 to 15 minutes:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

If a window asks to install the "command line developer tools", select
**Install** and wait for it to finish.

**2. Make Homebrew available in every new terminal.** On Macs with Apple
chips (M1 and later), Homebrew lives in a folder the terminal does not search
by default. This command adds it when needed and does nothing on Intel Macs:

```sh
if [ -x /opt/homebrew/bin/brew ]; then echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile; fi
```

**Close the Terminal window and open a new one** so the change takes effect.

**3. Install the tools:**

```sh
brew install git python node cmake ninja ccache cjson cppcheck
```

That is everything for macOS. Continue with
[step 2](#2-check-that-the-tools-are-installed).

<a id="linux"></a>

### Linux (Ubuntu or Debian)

These commands are for Ubuntu 24.04 or newer and Debian 12 or newer. Other
distributions work too if you install the same tools with their own package
manager.

**1. Install the system tools.** This is one long command:

```sh
sudo apt-get update && sudo apt-get install -y git wget curl ca-certificates flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 build-essential libcjson-dev cppcheck libdigest-sha-perl
```

**2. Install a current Node.js.** The Node.js that Ubuntu and Debian ship is
often too old for the project. These three commands add the official
NodeSource package source for the current long-term-support (LTS) release,
then install it:

```sh
curl -fsSL https://deb.nodesource.com/setup_lts.x -o /tmp/nodesource_setup.sh
```

```sh
sudo -E bash /tmp/nodesource_setup.sh
```

```sh
sudo apt-get install -y nodejs
```

**3. Allow your user to use USB serial ports.** Linux only lets members of
the `dialout` group talk to boards like this one:

```sh
sudo usermod -aG dialout "$USER"
```

This takes effect the next time you sign in. **Sign out and back in** (or
restart the computer) before step 7. On Windows with WSL, see the note at the
end of the Windows section instead.

That is everything for Linux. Continue with
[step 2](#2-check-that-the-tools-are-installed).

<a id="windows"></a>

### Windows (WSL2)

You need Windows 10 version 2004 or newer, or Windows 11, and an account that
can approve administrator prompts.

**1. Open PowerShell as administrator.** Open the Start menu, type
`PowerShell`, right-click **Windows PowerShell**, choose **Run as
administrator**, and select **Yes** when Windows asks.

**2. Install WSL2 with Ubuntu 24.04.** In that PowerShell window, run:

```powershell
wsl --install -d Ubuntu-24.04
```

When it finishes, **restart Windows**, even if it does not ask you to.

**3. Finish setting up Ubuntu.** After the restart, an **Ubuntu 24.04** window
opens by itself; if it does not, open **Ubuntu 24.04** from the Start menu.
The first time, it asks you to create a **username** and **password** for
Ubuntu. They have nothing to do with your Windows account. Choose something
you will remember: every `sudo` command asks for this password, and it does
not appear while you type.

**4. Install usbipd-win**, the tool that connects the board's USB port to
Ubuntu. In a PowerShell window (it does not need to be administrator), run:

```powershell
winget install --interactive --exact dorssel.usbipd-win
```

Accept the installer and Windows' permission prompt. You use it in step 7.

**5. Run the Linux commands inside Ubuntu.** From here on, every command in
this guide goes into the **Ubuntu 24.04** window, unless it says PowerShell.
Now run the three numbered parts of the [Linux section](#linux) above, in
order, in the Ubuntu window.

For the `dialout` change in Linux part 3 to take effect, run this in
**PowerShell**, then open **Ubuntu 24.04** again from the Start menu:

```powershell
wsl --shutdown
```

> [!IMPORTANT]
> Keep the project inside Ubuntu's own home folder, as the next steps do. Do
> not put it on the Windows drive (`/mnt/c/...`) and do not download it with
> Git for Windows: builds there are very slow and break in confusing ways.

## 2. Check that the tools are installed

Run each line. Each one should print a version number, not an error such as
`command not found`:

```sh
git --version
```

```sh
python3 --version
```

```sh
node --version
```

```sh
npm --version
```

```sh
cmake --version
```

What to look for:

| Command | Good result looks like | Needed |
| --- | --- | --- |
| `git --version` | `git version 2.43.0` | any recent version |
| `python3 --version` | `Python 3.12.3` | 3.10 or newer |
| `node --version` | `v26.13.1` | 22 or newer |
| `npm --version` | `11.20.0` | comes with Node.js |
| `cmake --version` | `cmake version 3.28.3` | 3.25 or newer |

Your numbers will differ; that is fine as long as they meet the last column.
If a command is missing or too old, repeat its part of step 1, or see
[Troubleshooting](#troubleshooting).

## 3. Download the project

This copies the project into a folder named `Open-Brew-by-Weight` inside your
home folder. First go to your home folder:

```sh
cd ~
```

Then choose **one** of these options.

**Option A: the latest version (recommended).** Downloads only the newest
code, without its history, which is the quickest:

```sh
git clone --depth 1 https://github.com/Cheerpipe/Open-Brew-by-Weight.git
```

**Option B: one specific version.** Every published version has a tag of the
form `vX.Y.Z`; the list is on the
[Releases page](https://github.com/Cheerpipe/Open-Brew-by-Weight/releases).
Replace `vX.Y.Z` with the tag you want, then run:

```sh
git clone --depth 1 --branch vX.Y.Z https://github.com/Cheerpipe/Open-Brew-by-Weight.git
```

If you choose an older version, follow the `docs/BUILD.md` inside the folder
you downloaded, because the tools it needs can differ from this guide.

Then go into the project folder and install the tools it uses to prepare the
web interface. `npm ci` installs the exact versions the project lists, and
takes a minute or two:

```sh
cd ~/Open-Brew-by-Weight
```

```sh
npm ci
```

From now on, **every `./scripts/dev` command runs from this folder**. If you
open a new terminal, go back into it first with `cd ~/Open-Brew-by-Weight`.

> [!TIP]
> Use `git clone` rather than GitHub's **Download ZIP** button: the build
> reads the version from Git, and a ZIP copy has no Git information.

### Getting a newer version later

The simplest way, which works with either option above, is to delete the
folder and download it again. Your controller's settings live on the
controller, not in this folder, so nothing is lost:

```sh
cd ~ && rm -rf ~/Open-Brew-by-Weight
```

Then repeat this step from the `git clone` command. Check the new version's
`docs/BUILD.md` too, in case it needs a newer ESP-IDF.

## 4. Install ESP-IDF

<a id="3-install-esp-idf-required"></a>

This firmware needs **ESP-IDF 6.1.1** exactly. These commands download it into
a folder named `esp/esp-idf-v6.1.1` inside your home folder, which is where the
project's scripts look for it, so keep that name. The download is about 2 to
3 GB and can take 10 to 30 minutes.

```sh
mkdir -p ~/esp
```

```sh
git clone -b v6.1.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.1.1
```

Then install the compiler and tools for the ESP32-S3 chip. This also creates
ESP-IDF's own private Python environment, so you never need to install Python
packages yourself:

```sh
~/esp/esp-idf-v6.1.1/install.sh esp32s3
```

It ends with a message such as `All done! You can now run: . ./export.sh`.
**You do not need to run that `export.sh` command:** the project's scripts
find and activate ESP-IDF on their own every time.

If the download or installation stops partway (for example, a network
hiccup), run the same command again; it continues where it left off.

## 5. Check that everything is ready

<a id="8-host-tests-before-you-flash"></a>

From the project folder, ask the project to check your setup. It only looks;
it never installs or changes anything:

```sh
cd ~/Open-Brew-by-Weight
```

```sh
./scripts/dev doctor
```

A healthy setup ends like this (paths and versions will differ on your
computer):

```text
PASSED: ./scripts/dev doctor
git: /usr/bin/git (git version 2.54.0)
python3: /opt/homebrew/bin/python3 (Python 3.15.2)
cmake: /opt/homebrew/bin/cmake (cmake version 4.5.0)
ctest: /opt/homebrew/bin/ctest (ctest version 4.5.0)
c++: /usr/bin/c++ (Apple clang version 21.0.0)
node: /opt/homebrew/bin/node (v26.13.1)
npm: /opt/homebrew/bin/npm (11.20.0)
cppcheck: /opt/homebrew/bin/cppcheck (Cppcheck 2.22.0)
ccache: /opt/homebrew/bin/ccache
ESP-IDF: /Users/you/esp/esp-idf-v6.1.1
IDF Python environment: auto-selected installed venv: /Users/you/.espressif/python_env/idf6.1_py3.15_env
```

What matters:

- The first line says **`PASSED`**.
- No tool says **`MISSING`**. If one does, install it as in step 1.
- The **`ESP-IDF:`** line shows the folder from step 4. If it says
  `not found`, repeat step 4.
- A line starting with `warning:` that suggests upgrading a tool is advice,
  not an error.

The project also has automatic self-tests for people who change the code.
You do not need them to build the firmware; they are described in
[Validate code changes](#validate-code-changes).

## 6. Build the firmware

<a id="5-build"></a>
<a id="4-ble-backend"></a>

Each build is made for one **board** and one **machine**, because the wiring
and the way the controller starts a shot differ between machines. You choose
them with two options:

- `--hardware` is the board. Every example in this guide uses the ESP32-S3
  Relay X1: `esp32-s3-relay-x1-speaker`.
- `--machine` is your espresso machine. This example uses the La Marzocco
  Linea Micra. [Step 8](#8-commands-for-each-machine) has the command for
  each supported machine.

```sh
./scripts/dev build --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra
```

The first build takes 5 to 15 minutes, needs an internet connection to fetch
some components, and prints a lot of text. Later builds are much faster.

Near the start it asks one question. **Press <kbd>Enter</kbd>** to accept the
suggested value:

```text
Extra compile flags [-Werror=deprecated-copy]:
```

A successful build looks like this (shortened):

```text
==> firmware-build: ./scripts/internal/firmware-idf build -- --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra
From CLI:                hardware_config=esp32-s3-relay-x1-speaker machine_config=la-marzocco-linea-micra
Defaults:                webui_language=en
Extra compile flags [-Werror=deprecated-copy]:
Activating ESP-IDF 6.1
ESP-IDF version: v6.1.1 (required: 6.1.1)
Building esp32-s3-relay-x1-speaker--la-marzocco-linea-micra (IDF, BLE=nimble, allocator=external)
WebUI language: en
[1/9] Performing build step for 'bootloader'
...
Project build complete. To flash, run:
 idf.py flash
...
sdkconfig: verified exact n16r8 flash/PSRAM, partition, MMU, rollback, watchdog, panic, GPTimer, IRAM placement, console, PSRAM ECC, and build-option profile
...
Build output folder: /Users/you/Open-Brew-by-Weight/build-idf/esp32-s3-relay-x1-speaker--la-marzocco-linea-micra
Flash-ready firmware: /Users/you/Open-Brew-by-Weight/build-idf/esp32-s3-relay-x1-speaker--la-marzocco-linea-micra/openbrewbyweight.bin
passed: summary artifacts/runs/20261125T114100Z-19421/summary.json
```

The last line, starting with **`passed`**, means the firmware is ready.

- **Ignore the `To flash, run: idf.py flash` hint** in the middle. It comes
  from ESP-IDF itself; always install with `./scripts/dev` as in step 7, which
  checks that the firmware matches your board first.
- Lines that mention a missing `versioned baseline` are informational and
  do not mean anything went wrong.

## 7. Install the firmware on the board

<a id="6-flash-usb"></a>

Installing is also called **flashing**. For the first installation:

1. Keep the board **disconnected from the espresso machine**. Do this on the
   desk, with only the USB cable connected.
2. Connect the board to the computer with the USB-C data cable. Plug it
   straight into the computer rather than into a hub.
3. Do the part for your system below, then run the install command.

**macOS:** the first time you connect the board, macOS may ask whether to
allow the accessory to connect. Select **Allow**.

**Linux:** make sure you signed out and back in after Linux part 3 of step 1.

**Windows:** connect the board to Ubuntu with usbipd-win. Keep an Ubuntu
window open while you do this.

1. In **PowerShell as administrator**, list the USB devices:

   ```powershell
   usbipd list
   ```

   Find the board: a line with `303a:1001` and a name such as
   `USB JTAG/serial debug unit` or `USB Serial Device (COM5)`. Note its
   **BUSID** in the first column, for example `2-3`.

2. Share it with WSL. This is needed once per board and USB socket; replace
   `2-3` with your BUSID:

   ```powershell
   usbipd bind --busid 2-3
   ```

3. Connect it to Ubuntu. Replace `2-3` again. `--auto-attach` reconnects the
   board every time it restarts, which happens during installation, so
   **leave this window open** until you finish:

   ```powershell
   usbipd attach --wsl --busid 2-3 --auto-attach
   ```

Now, in the terminal (Ubuntu window on Windows), from the project folder, run
the install command:

```sh
./scripts/dev flash --confirm --erase-all --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra
```

What the options mean:

- `--confirm` says you really want to write to the board. The command refuses
  to touch hardware without it.
- `--erase-all` wipes the board completely first. **Use it for the first
  installation**: a new board comes with other firmware that must go. It
  also deletes any settings, so leave it out for later USB updates (see
  below).
- Use the same `--hardware` and `--machine` that you built with in step 6.

The script looks for the board and suggests its port, for example:

```text
Detected USB ports: /dev/cu.usbmodem2101
Press Enter to use the suggested port, or type another path.
Serial port [/dev/cu.usbmodem2101]:
```

**Press <kbd>Enter</kbd>.** The port is named `/dev/cu.usbmodem...` on macOS
and `/dev/ttyACM0` (or a similar number) on Linux and WSL.

If it says `No USB CDC device detected`, put the board in installation mode
and run the command again:

1. Press and hold the **BOOT** button on the board.
2. While holding it, press and release the **RST** button.
3. Release **BOOT**.

When installation finishes, the last lines look like this (your numbers will
differ), and the board restarts on its own:

```text
Wrote 2810608 bytes (1612345 compressed) at 0x00070000 in 18.2 seconds (1232.1 kbit/s).
Hash of data verified.

Hard resetting via RTS pin...
```

If the board does not seem to start, press **RST** once. It is normal for the
USB port to disappear after the restart: the installed firmware keeps its USB
console off.

The board now creates its own Wi-Fi network for setup. Continue with
[Connect it to your Wi-Fi](QUICKSTART.md#5-connect-it-to-your-wi-fi), and
read [Hardware](HARDWARE.md) before you wire it into the machine.

### Updating later over USB

To install a newer build over USB **and keep your settings**, use the same
command without `--erase-all`. Because the USB port is off while the firmware
runs, first put the board in installation mode with **BOOT** and **RST** as
described above:

```sh
./scripts/dev flash --confirm --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra
```

Updating over Wi-Fi is usually easier: see the next step.

## 8. Commands for each machine

<a id="9-update-over-wi-fi-ota"></a>

Every command below uses the ESP32-S3 Relay X1 board. Each machine has two
commands:

- **First installation over USB** builds the firmware and installs it in one
  go, wiping the board. Prepare the board as in [step 7](#7-install-the-firmware-on-the-board).
- **Update over Wi-Fi (OTA)** builds the firmware and sends it to a
  controller that already runs Open Brew by Weight, keeping all its settings.

For a Wi-Fi update:

- The controller must be connected to your home Wi-Fi, and your computer must
  be on the same network.
- Replace `192.168.1.50` with your controller's address. The setup page showed
  it at the end; on macOS and most Linux desktops,
  `openbrewbyweight.local` also works (not on Windows with WSL).
- The command asks for your **device password**, the one you chose during
  setup. It does not appear while you type.
- After uploading, it shows which firmware it sent and asks
  `Commit and reboot the controller? [y/N]`. Type `y` and press
  <kbd>Enter</kbd>. The controller restarts with the new firmware when the
  machine is not brewing, and the command waits until it confirms.

[OTA](features/ota.md) explains Wi-Fi updates in detail.

### La Marzocco Linea Micra

The reference machine: this is the combination the project is developed and
tested on.

First installation over USB:

```sh
./scripts/dev build flash --confirm --erase-all --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra
```

Update over Wi-Fi:

```sh
./scripts/dev build ota --confirm --wait-for-confirmation --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra --host 192.168.1.50
```

### La Marzocco Linea Mini

> [!CAUTION]
> Experimental. This build has never been tested on a real Linea Mini, and
> there is no wiring guide for it yet. Its cloud features need the machine's
> IoT board. See [Experimental builds](BUILD_PROFILES.md#experimental-builds-linea-mini-r-and-linea-mini).

First installation over USB:

```sh
./scripts/dev build flash --confirm --erase-all --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-mini
```

Update over Wi-Fi:

```sh
./scripts/dev build ota --confirm --wait-for-confirmation --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-mini --host 192.168.1.50
```

### La Marzocco Linea Mini R

> [!CAUTION]
> Experimental. This build has never been tested on a real Linea Mini R, and
> there is no wiring guide for it yet. See
> [Experimental builds](BUILD_PROFILES.md#experimental-builds-linea-mini-r-and-linea-mini).

First installation over USB:

```sh
./scripts/dev build flash --confirm --erase-all --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-mini-r
```

Update over Wi-Fi:

```sh
./scripts/dev build ota --confirm --wait-for-confirmation --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-mini-r --host 192.168.1.50
```

### Rancilio Silvia Pro X

The Silvia Pro X has two builds. Pick the one that matches your installation:

- **With a reed switch** (recommended): you fitted the optional
  [reed switch](HARDWARE.md#reed-switch-optional-strongly-recommended-for-momentary-machines),
  which lets the controller see whether the machine is really brewing. It is
  the same board, but `--hardware` changes too, to
  `esp32-s3-relay-x1-speaker-reed`, because the reed switch uses one more
  input.
- **Without a reed switch:** the board only presses the machine's button.

With a reed switch, first installation over USB:

```sh
./scripts/dev build flash --confirm --erase-all --hardware esp32-s3-relay-x1-speaker-reed --machine rancilio-silvia-pro-x-reed
```

With a reed switch, update over Wi-Fi:

```sh
./scripts/dev build ota --confirm --wait-for-confirmation --hardware esp32-s3-relay-x1-speaker-reed --machine rancilio-silvia-pro-x-reed --host 192.168.1.50
```

Without a reed switch, first installation over USB:

```sh
./scripts/dev build flash --confirm --erase-all --hardware esp32-s3-relay-x1-speaker --machine rancilio-silvia-pro-x
```

Without a reed switch, update over Wi-Fi:

```sh
./scripts/dev build ota --confirm --wait-for-confirmation --hardware esp32-s3-relay-x1-speaker --machine rancilio-silvia-pro-x --host 192.168.1.50
```

To list every board and machine the project knows, and which go together:

```sh
./scripts/dev profiles
```

## Troubleshooting

Find the message you see, or the closest description. Most problems come from
a step that was skipped, a terminal that was opened before a change, or a USB
cable.

### Commands are not found

**`zsh: command not found: brew` (macOS).** The terminal does not know where
Homebrew is. Run macOS part 2 of step 1 again, then close and reopen Terminal.

**`xcrun: error: invalid active developer path` (macOS).** Apple's Command
Line Tools are missing, often after a macOS upgrade. Install them, then try
again:

```sh
xcode-select --install
```

**`command not found` for `git`, `node`, `python3` or `cmake`.** That tool was
not installed, or the terminal was opened before it was. Close the terminal,
open a new one, and run step 2 again. If it is still missing, repeat its part
of step 1.

**`./scripts/dev: No such file or directory`.** You are not in the project
folder. Run `cd ~/Open-Brew-by-Weight` and try again.

**`Permission denied` when running `./scripts/dev`.** The project was probably
downloaded as a ZIP or copied from Windows. Delete the folder and download it
again with `git clone` as in step 3.

### Python problems

**`error: externally-managed-environment` after a `pip install`.** You do not
need `pip` for this guide, and you should not install Python packages by hand
or with `sudo pip`. ESP-IDF keeps its own private Python environment inside
`~/.espressif/python_env`, created by `install.sh` in step 4.

**Builds fail after you ran `source .../activate` or use Anaconda/Miniconda.**
Another Python environment is active and gets in ESP-IDF's way. Your prompt
often shows its name in brackets, such as `(base)` or `(venv)`. Leave it and
try again:

```sh
deactivate 2>/dev/null; conda deactivate 2>/dev/null; true
```

To stop Anaconda from activating itself in every new terminal:

```sh
conda config --set auto_activate_base false
```

**ESP-IDF stopped working after a system or Homebrew update.** Updates
sometimes install a newer Python, which leaves ESP-IDF's private environment
behind. The scripts repair the common cases by themselves. If the error
persists, run the ESP-IDF installer again; it keeps what still works:

```sh
~/esp/esp-idf-v6.1.1/install.sh esp32s3
```

### ESP-IDF problems

**`ESP-IDF: not found` from `doctor`, or `ESP-IDF not found (idf.py /
export.sh)` from a build.** ESP-IDF is not in `~/esp/esp-idf-v6.1.1`. Check
that the folder exists and has exactly that name:

```sh
ls ~/esp
```

If it is missing or named differently, repeat step 4.

**`ESP-IDF 6.1.1 is required`.** Another ESP-IDF version is active in this
terminal, for example from an IDE extension or an `export.sh` you ran by
hand. Open a new terminal, go to the project folder, and build again without
running any `export.sh`. If you added an ESP-IDF line to your `~/.zshrc` or
`~/.bashrc`, remove it.

**`install.sh` stops with a download or certificate error.** Usually a
temporary network problem, a VPN or a company proxy. Run it again on a
different network if possible; it resumes where it stopped.

### Node.js and `npm ci` problems

**`npm ci` fails with `EBADENGINE`, `Unsupported engine` or syntax errors.**
Node.js is too old. Check with `node --version`: you need 22 or newer. On
Linux and WSL, repeat Linux part 2 of step 1; on macOS, run
`brew upgrade node`.

**`npm ci` fails with `EACCES` or `permission denied`.** Never run `npm` with
`sudo`. If you did, the folder now belongs to the administrator. The easiest
fix is to download the project again as in
[Getting a newer version later](#getting-a-newer-version-later).

**`npm ci` says it cannot find `package.json`.** You are not in the project
folder. Run `cd ~/Open-Brew-by-Weight` first.

**A build says `node not found on PATH` or `Cannot find module 'terser'`**
(or another module). Node.js is missing or `npm ci` was not run. Repeat the
end of step 3.

### Build problems

**The build seems stuck at `Extra compile flags [...]:`.** It is waiting for
you. Press <kbd>Enter</kbd>.

**The first build fails while downloading components.** The first build needs
internet access. Check your connection and run the same command again.

**`IDF component lock changed during build`.** Files in the project were
changed, which can happen after experimenting. Put them back as they were
downloaded and build again:

```sh
git checkout -- idf/dependencies.lock
```

**`No space left on device`.** Free some disk space: the toolchain and the
build folders together use about 10 GB.

### USB and installation problems

**`No USB CDC device detected`, or the board never shows up.** Work through
these in order:

1. Use another USB-C cable: many cables can only charge.
2. Plug it straight into the computer, not into a hub, monitor or dock.
3. Put the board in installation mode: hold **BOOT**, press and release
   **RST**, release **BOOT**. Then run the command again.
4. On Windows, check that the `usbipd attach ... --auto-attach` window is
   still running, and that an Ubuntu window is open.

**`Installed partition table is not the current n16r8 project layout`.** The
board has other firmware with a different memory layout, which is always the
case for a new board. Add `--erase-all` to the command. This erases the board
completely, including any saved settings.

**`Permission denied: '/dev/ttyACM0'` (Linux and WSL).** Your user is not
in the `dialout` group yet. Repeat Linux part 3 of step 1, then sign out and
back in. On WSL, run `wsl --shutdown` in PowerShell and reopen Ubuntu. Check
with this command, whose output must include `dialout`:

```sh
groups
```

**`Device or resource busy`, or the port disappears in the middle of the
installation (Linux).** Another program is using the port. Close the Arduino
IDE, other serial monitors, and 3D-printing software such as Cura or
PrusaSlicer. On Ubuntu, the ModemManager service can also grab new USB
devices; stop it for this session:

```sh
sudo systemctl stop ModemManager
```

**The board works on macOS but nothing happens when you plug it in.** On Macs
with Apple chips, a laptop may block new accessories until you allow them.
Unplug and reconnect the board and look for the **Allow accessory to
connect** prompt, or check **System Settings → Privacy & Security → Allow
accessories to connect**.

**Windows: does the board need a USB driver?** No. The ESP32-S3 on this
board has USB built into the chip, and Windows 10 and 11 already include the
driver for it: it appears as `USB Serial Device (COMx)` or
`USB JTAG/serial debug unit`. With WSL the board is handed to Ubuntu anyway,
so Windows drivers are not involved. CP210x or CH340 drivers, which many
tutorials mention, are for other ESP32 boards with a separate USB chip; this
board does not need them. If Windows shows an **Unknown USB device**, try
another cable or socket.

**Windows: `usbipd: error: Device is not shared`.** Run the `usbipd bind`
command from step 7 in PowerShell as administrator first.

**Windows: `usbipd: error: There is no WSL 2 distribution running`.** Open the
Ubuntu 24.04 window, then run `usbipd attach` again.

**Windows: `wsl --install` fails with `0x80370102` or a message about
virtualization.** Virtualization is turned off in your computer's firmware
(BIOS/UEFI). Turn on the option called **Intel VT-x**, **Intel Virtualization
Technology**, **AMD-V** or **SVM Mode**, and run the command again. Your
computer maker's support site explains how to open those settings.

**After installation the board does not start.** Press **RST** once. If it
still does nothing, unplug and reconnect the USB cable. The board's USB port
disappearing after the restart is normal.

### Wi-Fi update (OTA) problems

**`Could not resolve host`, `Connection timed out` or `No route to host`.**

- Use the controller's IP address instead of `openbrewbyweight.local`. On
  Windows with WSL, `.local` names do not work.
- Check that the computer and the controller are on the same network. Guest
  Wi-Fi networks often keep devices apart.
- On macOS, the first connection from **Visual Studio Code**, **iTerm** or
  another app's terminal may show a **Local Network** permission prompt.
  Allow it, or turn the app on under **System Settings → Privacy & Security →
  Local Network**. The built-in Terminal does not need this.

**The password is rejected.** It is the device password you chose during
setup, not your Wi-Fi password. If you forgot it, see
[Recover Wi-Fi, AP, and Password](EMERGENCY_RECOVERY.md#recover-wi-fi-ap-and-password).

**`Non-interactive OTA requires --yes`.** The command was run where it
cannot ask questions, such as from a script. Run it in a normal terminal, or
see the OTA options in [For advanced users](#update-over-wi-fi-ota).

## For advanced users

Everything below is reference material for customizing the build, working on
the code, or understanding what the scripts check. None of it is needed to
build and install the firmware with the standard settings.

### Exact tool versions

Supported firmware uses ESP-IDF **6.1.1** (release tag **v6.1.1**),
Arduino-ESP32 **4.0.0** as an IDF component, and native NimBLE. The bundled
EspressoScaleBLE library is not installed through Library Manager.

| Dependency | Local version contract | Source of truth |
| --- | --- | --- |
| Node.js | No pin; any current release resolved from `PATH` | [`./scripts/dev doctor`](../scripts/dev) |
| Web UI packages | Exact direct and resolved versions via `npm ci` | [`package.json`](../package.json), [`package-lock.json`](../package-lock.json) |
| ESP-IDF | Exactly 6.1.1 (tag v6.1.1) | This guide and the build scripts |
| IDF components | Locked graph, including mDNS 1.14.1 and ESP WebSocket client 1.8.0 | [`idf/dependencies.lock`](../idf/dependencies.lock) |
| Cppcheck | No pin; any current release resolved from `PATH` | [`./scripts/dev doctor`](../scripts/dev) |
| Host CMake | 3.25 or newer | [`CMakePresets.json`](../CMakePresets.json) schema 6 |

Git, Node.js, Cppcheck, the host compiler, Ninja, cJSON and the system Python
have no separate project pin; use versions compatible with the requirements
above and verify them with the tests below. ESP-IDF manages its own Python
environment. Cppcheck is only needed for [static analysis](STATIC_ANALYSIS.md).
The [Home Assistant integration](https://github.com/Cheerpipe/ha-open-brew-by-weight)
lives in its own repository with its own toolchain; it is not needed for
firmware builds.

<a id="2-nodejs-dependencies"></a>

`npm ci` is an explicit setup step. Test commands never install dependencies.

Native Windows builds are not supported: the build scripts need Bash, and the
per-variant build lock uses POSIX `fcntl`, which the Windows Python does not
provide. Use WSL2 as described above. The Windows notes in
[Static analysis](STATIC_ANALYSIS.md#4-windows-prerequisites-native-no-wsl)
cover analysis-tool preparation only.

### ccache

`ccache` is optional. Firmware builds use it automatically when it is
installed, and rebuilds after a flag or configuration change are much faster.
Every validated build tree adds about 85 MB to the cache, so give it room with
`ccache -M 25G`; `./scripts/dev doctor` warns when it is nearly full. Git
worktrees inside the main checkout share the cache, so a new worktree reuses
what the others compiled. Generated code is the same either way; only debug
information may name the worktree that compiled an object first.

### Other ways to provide ESP-IDF

The scripts look for ESP-IDF in this order:

1. **An active environment.** It is reused only when `IDF_PATH`, its
   `IDF_PYTHON_ENV_PATH/bin/python`, `idf.py`, and the reported 6.1.1 version
   all agree. A stale or mismatched active environment is discarded before
   the fallbacks below.
2. **`IDF_PATH` pointing at an inactive SDK.** The scripts source its
   `export.sh`.
3. **`$HOME/esp/esp-idf-v6.1.1`**, the location used in step 4.

Every path rejects versions other than 6.1.1.

The [ESP-IDF Installation Manager (EIM)](https://docs.espressif.com/projects/idf-im-ui/en/latest/)
also works: install ESP-IDF **6.1.1**, open a fresh shell, and source the
activation script EIM prints (or use **Open IDF Terminal** in its GUI) before
running `./scripts/dev`. Use the actual filename EIM created, for example:

```sh
source "$HOME/.espressif/tools/activate_idf_v6.1.1.sh"
idf.py --version
```

When an SDK's own activation script points at a Python environment that no
longer exists on the machine (for example after a macOS or Python upgrade),
the scripts pick up an installed environment for the same 6.1 release from
`~/.espressif/python_env` automatically, preferring the newest one. If none
matches, the activation error names the missing directory so you can run the
install script for it.

`idf/main/idf_component.yml` and `idf/dependencies.lock` define the component
graph, including Arduino-ESP32 4.0.0, mDNS 1.14.1 and ESP WebSocket client
1.8.0. The WebSocket component supplies selectable Micra WebSocket
observation; authentication and commands remain HTTP. The build fails if
dependency resolution changes the lockfile; review and commit such updates
separately. First firmware builds may need network access to resolve SDK
components; prepare these dependencies before attempting an offline
validation run. Host tests and firmware compilation need no running Home
Assistant service or connected controller.

### Upgrade dependencies deliberately

Run `npm ci` for ordinary setup; it does not update the lockfile. To upgrade a
Web UI package, choose its version with
`npm install --save-dev --save-exact <package>@<version>` and review both npm
files.

An ESP-IDF upgrade must change the local version guard and the generated
component lock together; regenerate the lock with the intended SDK version,
then run the full build gate. Do not hand-edit lockfiles. With that SDK
active, a scratch build directory keeps the regeneration out of the project
tree:

```sh
idf.py -C idf -B /tmp/obbw-lock -D SDKCONFIG=/tmp/obbw-lock/sdkconfig \
  -D IDF_TARGET=esp32s3 update-dependencies
```

An Arduino-ESP32 upgrade also needs a review of the components its manifest
adds. Dependencies the firmware never uses (Modbus, RainMaker, Insights,
speech recognition) are replaced in `idf/main/idf_component.yml` with empty
stubs in `idf/third_party/`; give a new unused dependency the same treatment
before accepting the lock.

### Validate code changes

`./scripts/dev` is the canonical entry point. The host and Web UI suites are:

```sh
./scripts/dev test normal
./scripts/dev test web
```

A passing host run ends with CTest's success summary; the command prints the
full-log/JSON summary location in `artifacts/runs/`. Missing tools, cJSON, or
Node dependencies are failures, not skipped tests.

The host suites are currently qualified with Apple clang on macOS only. With
GCC on Linux (Ubuntu 24.04, GCC 13) `test normal` does not compile yet: a
host stub header relies on a transitive `<functional>` include, and GCC's
`-Wclass-memaccess` is promoted to an error. Firmware builds are not
affected.

For changed code, run `./scripts/dev classify` and the complete gate from
[VALIDATION.md](../VALIDATION.md), including sanitizers/builds where required.
Host tests cannot verify wiring, radio timing, or physical stop behavior.
R2/R3 validation compiles every supported profile with the `--development`
profile, including the Linea Micra pair, in its own `build-gate/` trees. It
does not change `build-idf/<hardware>--<machine>/`, so run
`./scripts/dev build` for the image you are going to install.

### Build profiles and outputs

A hardware profile selects the assembled board, memory layout, GPIO wiring,
relay polarity, and installed peripherals; a machine profile selects its
control topology and factory defaults. See
[Hardware and machine build profiles](BUILD_PROFILES.md) for the complete
contracts and compatibility rules.

Both options are required together. Each selector accepts an exact built-in ID
or an explicit JSON path. There is no implicit or remembered profile
selection, and an architecture-only build is rejected. `--arch`, when also
supplied, can only confirm the target derived from hardware. The X1 profile
always resolves to **n16r8** (16 MB flash / 8 MB OPI PSRAM).

A build writes to `build-idf/<hardware>--<machine>/` and also creates a `.bin`
whose filename contains that complete variant. Every successful `build` also
preserves the matching `openbrewbyweight.elf` and `openbrewbyweight.bin` under
`artifacts/firmware/<hardware>--<machine>/<ELF SHA>/`, alongside
`identity.json`. The SHA is the one printed in an ESP32 panic log; its prefix
can locate the matching archive. These Git-ignored copies are created without
flashing and are retained by `./scripts/dev clean`. A later build cannot
replace an archive whose SHA already exists with different files.

Machine defaults seed a new installation and factory reset. Valid persisted
settings survive ordinary boot and OTA. Firmware identity is checked before
writing the device. Saved data and the partition table use the v1 layout:
installing v1 over any earlier development image needs the
[clean installation](#clean-installation) with `--erase-all`; OTA and USB
updates between v1 builds work normally.

The served Web UI is machine-type exclusive: a build compiled for a paddle
machine ships only the paddle controls, a momentary build ships only the
switch timings and the forced-pulse action, and a reed build additionally
exposes the reed confirmation window while leaving out the idle/brewing state
controls that only machines without a reed sensor need. Controls for other
machine types are not embedded at generation time, so each firmware binary
carries only the interface its machine can use. The same applies to optional
features: the Linea Micra cloud settings, Home machine rows, and diagnostics
ship only in Linea Micra builds, and the Home remote control bar only in
builds compiled with remote machine control.

The build renders the selected catalog into
`build-idf/<hardware-id>--<machine-id>/generated/OpenBrewByWeightWebAssetsGzip.cpp`,
generates version identity and
`build-idf/<hardware-id>--<machine-id>/openbrewbyweight.bin`, then checks image
and memory budgets. Use only the image for the intended profile pair.

The wrapper preserves project diagnostics but hides ESP-IDF 6.1's known
`esp_wifi`/`wpa_supplicant` component-validation warnings. Project-owned
components compile with `-Wall -Wextra -Wunused-parameter`, and any warning
they emit fails the build.

### Power management defaults

Production defaults compile DFS (`CONFIG_PM_ENABLE`) and S3 controller modem
sleep with the main crystal. The runtime
[Power management](settings/power-management.md) setting defaults off. The
build wrapper recreates cached configurations missing this support; automatic
light sleep remains disabled.

### Web UI language

The Web UI is compiled in English by default. To select it explicitly:

```sh
./scripts/dev build \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --webui-language EN
```

Language selection happens while the Web assets are generated; it does not add
a browser setting or embed every catalog. The flag overrides
`OPENBREWBYWEIGHT_WEBUI_LANGUAGE`, and an omitted selection always uses `en`.
Codes are case-insensitive, `_` is normalized to `-`, and a regional code
tries its exact catalog followed by its base language. English is currently
the only shipped catalog, so another base language fails instead of silently
producing English. Adding a complete locale file does not affect existing
firmware until that locale is selected. A catalog holds only words to
translate: icons, numbers, punctuation and example addresses stay in the Web
UI sources, and the build rejects a catalog entry without any letters.

### Release, development and custom flags

Omitting the profile, or passing `--release` explicitly, builds without admin
unlock and without the JTAG console. Pass `--release` explicitly for an image
you publish: it refuses to build unless the Git tree is clean, untracked files
included, so the image's version never carries `-dirty`.

For local development, use the development build profile
(`--no-auth-admin --jtag` combined). `--no-auth-admin` and `--jtag` also work
individually on top of a release build.

```sh
./scripts/dev build \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --development
```

Explicit `--flags` are applied after the JSON values. Supported profile values
are reflected in the resolved manifest; additive compiler flags keep working.
The resolver rejects unsafe or contradictory overrides, including GPIO
collisions, a machine that requires absent reed hardware, a normally-closed
relay profile, or a different `--arch`. Development mode remains CLI-only and
must never be added to a profile.

`--flags` applies to one build only and is never saved in `.openbrewbyweight`;
a value left there by older scripts is ignored. When `--flags` is omitted in
an interactive terminal, the build asks for it and suggests the default
`-Werror=deprecated-copy`. `--flags` cannot turn on a development feature by
itself: `OPEN_BREW_BY_WEIGHT_DEVELOPMENT=1` needs `--no-auth-admin` (or
`--development`), `OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1` needs `--jtag`, and
remote machine control is accepted only from `--flags` on the command line,
not from `OPENBREWBYWEIGHT_FLAGS`. Each of these defines takes only `=0` or
`=1`. The build verifier also rejects a retained `menuconfig` choice that
enables admin unlock, remote machine control, or the USB console without
`--jtag`.

Machine type is derived from `interface.control` plus `interface.feedback`; a
conflicting `OPEN_BREW_BY_WEIGHT_MACHINE_TYPE` override is rejected.

| Option | Meaning |
| --- | --- |
| `OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=0/1/2` | Paddle / momentary / momentary+reed; see [machine types](HARDWARE.md#machine-types). |
| `OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=0/1` | Omit / include local passive buzzer. Follows the hardware profile's `speaker.present` by default; `=0` omits it even when a speaker is present. |
| `OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1` | Development USB Serial/JTAG at boot without the GPIO4 console jumper; build it with `./scripts/dev build --jtag` (or the `--development` profile). |
| `OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=0/1` | Remote start/rinse disabled / explicit opt-in. Default is disabled; opt in for one build with `--flags -DOPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1` on the command line. Builds without it also omit the Home remote-action controls from the served interface. |
| `OPEN_BREW_BY_WEIGHT_DEVELOPMENT=1` | Compiles administration as public: Admin and Diagnostic need no device-password session and the UI shows no lock panel. The password-unlock endpoints are not compiled in this mode. Build it with `--no-auth-admin` or the `--development` profile. Local development only — never use for an installed machine. |

### Compiler optimization and existing sdkconfig

Supported ESP-IDF firmware builds default to
`CONFIG_COMPILER_OPTIMIZATION_PERF=y` from `idf/sdkconfig.defaults`, which
selects GCC `-O2` for every built-in hardware and machine profile. After
compiling, the build verifier prints the selected optimization level
(`sdkconfig: CONFIG_COMPILER_OPTIMIZATION_PERF=y` on default builds) and fails
if the configuration does not hold it. The `Debug` setting in the root
`CMakePresets.json` applies only to host tests; it does not change firmware
optimization.

ESP-IDF offers exactly four optimization levels; `-O1` and `-O3` are not
selectable and the wrapper rejects them. To compile an experiment at another
level, pass exactly one of the transient build options; omitting them keeps
the default `-O2` build. For example, a size-optimized build:

```sh
./scripts/dev build --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra --os
```

| Option | GCC level | Kconfig choice |
| --- | --- | --- |
| `--o0` | `-O0` | `CONFIG_COMPILER_OPTIMIZATION_NONE` |
| `--og` | `-Og` | `CONFIG_COMPILER_OPTIMIZATION_DEBUG` |
| `--o2` | `-O2` | `CONFIG_COMPILER_OPTIMIZATION_PERF` (default) |
| `--os` | `-Os` | `CONFIG_COMPILER_OPTIMIZATION_SIZE` |

The options are mutually exclusive, apply to that invocation only, and are
never persisted. They work through a generated defaults file appended to
`SDKCONFIG_DEFAULTS`; when an existing sdkconfig holds a different level, the
build recreates the configuration so the requested level takes effect, which
discards local `menuconfig` choices stored in that file. A later build without
a level option restores `-O2` the same way. The versioned image and memory
baselines were measured with `-O2`: one shared baseline for `--development`
builds and one per supported pair for the flag-free release image (see
[resource budgets](P2_RESOURCE_BUDGETS.md)). Other optimization levels and
builds with extra flags still check the OTA partition limit, external BSS
ceiling, and required internal-memory placement; their image sizes do not
establish a versioned resource baseline.

The final verifier also rejects drift from the qualified production profile:
the supported build uses 16 MB flash, `partitions-n16r8.csv`, and OCT PSRAM,
and requires QIO/80 MHz flash, 80 MHz PSRAM, a 32 KiB internal reserve, a
2 KiB malloc threshold above which `malloc()` prefers PSRAM, 64 KiB MMU pages,
rollback support, mDNS dynamic responder allocations in PSRAM, and an
internal-RAM mDNS task stack because flash core dumps cannot read a stack in
cache-backed PSRAM — plus the pinned boot/task/interrupt watchdog and panic
settings, and the GPTimer ISR handler in IRAM, while the unused RMT driver and
the GDMA control and interrupt code stay in flash. Other application,
NimBLE/VHCI, HTTP, persistence, control, and flash-writing stacks remain
internal. These checks verify current hardware settings; they do not retune
clocks, partitions, or watchdog durations.

The supported build maps flash code and read-only data into PSRAM at startup
and executes from there, reserving roughly 1.9 MiB of PSRAM ahead of the heap.
Because code runs from PSRAM, flash writes and erases keep the caches enabled;
ESP-IDF still pauses them while a flash memory mapping is open, such as during
OTA image verification. A successful build verifies configuration and memory
limits; only an on-device comparison can establish whether loop gaps improve.

Each build compares its selected repository defaults and build profile with
the last verified build of that variant. If an input changed, was added, or
was removed, it regenerates `sdkconfig` and checks the effective configuration
before accepting the firmware image. An unchanged variant keeps its existing
configuration for incremental builds. Regeneration discards local
`menuconfig` choices in that variant, so record any custom choices before
changing defaults. Inspect one current tree with:

```sh
grep '^CONFIG_COMPILER_OPTIMIZATION' \
  build-idf/esp32-s3-relay-x1-speaker--rancilio-silvia-pro-x/sdkconfig
```

To restore repository defaults manually after changing `menuconfig`, rebuild
the selected variant with `--force-sdkconfig-regenerate`. This discards its
local `menuconfig` choices without cleaning the whole build tree:

```sh
./scripts/dev build --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra --force-sdkconfig-regenerate
```

Use the directory produced by the exact selected pair. The regeneration flag
applies only to commands that include `build`; standalone flash, OTA, and
monitor reject it. Do not add optimization flags through `--flags`; the
supported optimization contract is the Kconfig selection above, which the
build verifier checks. The `menuconfig` route for the optimization level is
gone: the wrapper recreates the configuration whenever the level differs from
the requested one.

Other choices in an existing IDF `sdkconfig` are retained until selected
build inputs change or regeneration is requested. A mismatch in a required
production setting stops the build and identifies the setting; use the
regeneration flag to restore defaults after a local `menuconfig` edit.
Omitting a macro does not always mean its feature is off; explicit flags
override matching choices. Review Diagnostic build identity and options before
installation.

### Partition layout

The supported partition layout has two 3.5 MiB app slots; arbitrary 4 MB
layouts cannot hold this firmware. The layout reserves 640 KiB for a temporary
ESP-IDF core dump, 1,408 KiB for two saved crash records and 4,016 KiB for
shot curves. A 1,340 KiB `reserved` area is left unused for future firmware.
The mDNS task stack is internal so that stack remains available to a core
dump.

### USB installation details

Proceed only after reviewing the image, board, wiring and applicable
[bench checks](MANUAL_TEST_PLAN.md). Keep the machine activation circuit
disconnected for initial verification. Use BOOT + RST for ROM download if the
app exposes no port. App serial monitoring needs the
[GPIO4 jumper or a JTAG build](HARDWARE.md#usb-console-jumper).

`--port` skips the port prompt; the last port used is remembered in the
Git-ignored `.openbrewbyweight` file and suggested only while it exists:

```sh
./scripts/dev flash --confirm --port /dev/cu.usbmodem2101 \
  --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra
```

Linux and WSL commonly use `/dev/ttyACM0`. Stages go before their options; add
`monitor` to open the serial console only after both earlier stages pass. A
`build ... monitor` sequence needs the JTAG console, so use `--development` or
`--jtag`:

```sh
./scripts/dev build flash monitor --confirm --development \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --port /dev/cu.usbmodem2101 --speed 115200
```

Build first whenever sources, options, or the Web UI language change: the
installer never rebuilds or relabels the selected image. Successful transfer
still requires post-boot and electrical verification.

The installer reads the partition-table sector first. Project builds always
use the existing `flash_args`, which transfers the bootloader, partition
table, initial OTA metadata and application without invoking a build.
`--no-check` only skips local identity verification. An external `--image`
still replaces app0 only, and only on the current installed layout.

### Clean installation

The installer flashes in place only when the controller's partition table is
blank or exactly matches `idf/partitions-<arch>.csv`: every partition must
agree in name, type, subtype, offset and size. Any other table, including
every development layout from before v1, is refused with a pointer to
`--erase-all`, so a near match never gets flashed over by accident. Installing
v1 over any earlier development image needs this clean installation, because
v1 does not read settings or history saved by earlier firmware. App-only
`--image` and OTA cannot change the partition table.

**`--erase-all` erases both firmware slots and all saved data:** settings,
Wi-Fi credentials and password, recipes and presets, calibration, scale
preferences, shot history and last shot. There are also no retained crash
records after this installation. Settings and curves start from factory
defaults without migration.

```sh
./scripts/dev flash --confirm --port /dev/cu.usbmodem2101 \
  --hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra \
  --erase-all
```

Record the settings you need before reinstalling. Do not combine
`--erase-all` with an external `--image`; the project build outputs are
required to write the bootloader, new partition table, initial metadata and
application. An app-only image cannot activate a changed partition table.

### Serial monitor

<a id="7-serial-monitor-and-cli"></a>

```sh
./scripts/dev monitor --port /dev/cu.usbmodem2101 --speed 115200
```

The monitor needs the [GPIO4 jumper or a JTAG build](HARDWARE.md#usb-console-jumper).
Exit with **Ctrl+]**. Send `HELLO` to check command access even when debug
logging is off. See [USB CLI](SERIAL_CLI.md) for troubleshooting and commands.
Verify boot identity, configured machine type and relay behavior, then
continue with [First setup and daily use](GETTING_STARTED.md).

### Update over Wi-Fi (OTA)

After installation, follow [OTA](features/ota.md) for upload, verification,
commit, confirmation and recovery. OTA cannot migrate the partition table.

`--yes` accepts the commit question without asking; `--wait-for-confirmation`
independently verifies the rebooted image. For scripts, supply the password
through `OPENBREWBYWEIGHT_DEVICE_PASSWORD` or `--password-stdin`, never as a
command-line argument:

```sh
./scripts/dev build ota --confirm \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --host 192.168.1.50 \
  --yes --wait-for-confirmation
```

See the [complete `dev` examples](SCRIPTS.md) for standalone and combined
build, flash, OTA, and monitor commands.

### One supported command interface

`./scripts/dev` is the only supported public firmware command. The old direct
IDF stage aliases and combination wrappers were removed. Focused shell files
under `scripts/internal/` are private implementation details.

### Optional static analysis

Use [Static analysis](STATIC_ANALYSIS.md) when requested or required by the
validation gate. It lists prerequisites, tool behavior and failure meanings.

### Firmware version

`VERSION` supplies the release number as exactly `MAJOR.MINOR.PATCH` (for
example `1.0.0`; `1.0`, `v1.0.0`, and leading zeros are rejected). The build
adds the git revision and a `-dirty` marker when tracked files changed or
untracked files exist. The Web UI footer and boot output identify the
resulting version; the ESP-IDF app descriptor reports the `VERSION` number.
The generator also records the time of each firmware build, shown in
Diagnostic and on the Firmware updated card, so every build relinks that one
file and is an image of its own, even with no source change.
