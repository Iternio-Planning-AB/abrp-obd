# Getting started

This guide takes you from a new Freematics ONE+ to a car that reports to ABRP on its own. Each step
ends with a **✔ Check** so you know it worked before moving on.

Before you start, check that your car is on the
[compatibility list](../README.md#will-it-work-with-my-car) and that you have everything under
[What you need](../README.md#what-you-need).

## Which path?

- **Hologram SIM, in Europe:** take the [quick path](#quick-path-flash-a-ready-made-image). You
  flash a ready-made image and skip installing a build toolchain.
- **Any other SIM, or outside Europe:** build the firmware yourself with your settings, steps 1–4.

Both paths continue with [step 5](#5-install-it-in-the-car).

## Quick path: flash a ready-made image

Every release comes with `factory.bin`, a complete image for a new dongle.

1. Download `factory.bin` from the latest release, under **Releases** on this repository's page.
2. Install [esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32/installation.html)
   (`pip install esptool`), or use Espressif's browser-based
   [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge instead.
3. Connect the dongle to your computer with a USB cable and flash the image to address `0x0`:

   ```sh
   esptool.py write_flash 0x0 factory.bin
   ```

   In esptool-js: connect, set the flash address to `0x0`, choose `factory.bin` and program.

`factory.bin` erases everything on the dongle, including any earlier pairing. That's what you want on
a new dongle; don't use it to update one that's already set up.

**✔ Check:** esptool finishes with `Hash of data verified`. Continue with
[step 5](#5-install-it-in-the-car).

## 1. Install PlatformIO

PlatformIO builds the firmware and copies it onto the dongle.

1. Install [Visual Studio Code](https://code.visualstudio.com/) and [git](https://git-scm.com/downloads).
2. Install the [PlatformIO extension](https://platformio.org/install/ide?install=vscode) for VS
   Code. When it's done, a PlatformIO icon (an alien head) appears in the left toolbar.

If you'd rather use the terminal, the [PlatformIO Core CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
works the same way; the commands are given below.

**✔ Check:** the PlatformIO icon is in the VS Code toolbar, or `pio --version` prints a version.

## 2. Get the code

Clone this repository with git: the **Code** button at the top of the repository page gives you the
URL. In VS Code, **Clone Git Repository** on the start page does the same. **Don't use "Download
ZIP":** the build reads its version number from the git history, and without it the firmware
reports version 0.

After cloning, fetch the version tags too:

```sh
git fetch --tags
```

Then open the `baseLogger` folder in VS Code (not the repository root). PlatformIO picks up
`platformio.ini` from there by itself.

**✔ Check:** PlatformIO shows an `esp32dev` project environment.

## 3. The SIM card

First, **activate the SIM and give it a data plan** in your provider's dashboard. A SIM that isn't
active connects to nothing, and it looks the same as every other failure.

The SIM must support **LTE-M** (also called Cat-M1), which is what the dongle's modem uses, with 2G
as a fallback. Many ordinary phone SIMs don't, so check with your provider.

**The APN.** The firmware is set up for a [Hologram](https://www.hologram.io/) SIM. For any other
SIM, find your provider's APN (the name of its data network, usually under "APN settings"). Open
`baseLogger/config.h`, find this line and replace `hologram` with your APN:

```c
#define CELL_APN "hologram"
```

**Outside Europe.** The modem only searches the European LTE-M bands (1, 3, 8, 20 and 28), to
connect faster. Elsewhere, change `CATM_BANDS` in `libraries/NetworkWrapper/cellwrapper.cpp` to your
carrier's LTE-M bands. Common US ones are 2, 4, 12, 13, 66 and 71; check with your carrier.

> **If you change either setting, don't install firmware updates from the app.** Official releases
> are built for Hologram in Europe and would put those settings back, and the dongle would lose its
> connection. Update by pulling the newer code and flashing over USB again (step 4).

**✔ Check:** the SIM is active, it works on LTE-M, and `CELL_APN` matches its APN.

## 4. Flash the firmware

1. Connect the dongle to your computer with a USB cable.
2. In VS Code, open the PlatformIO panel and run **esp32dev → Upload and Monitor**. From the
   terminal:

   ```sh
   cd baseLogger
   pio run -e esp32dev -t upload -t monitor
   ```

The first build downloads the ESP32 toolchain and takes a few minutes. After that, the upload takes
about a minute and the serial monitor opens.

**✔ Check:** the monitor shows a line like

```
=== FW BUILD: v0.1.0  v1000  (compiled ...) ===
```

The version is your checkout's (it may have a `+…` suffix if you're past the newest release). Then
shortly after:

```
No persisted vehicle profile - polling disabled until a profile is pushed
```

That second line is expected: the dongle doesn't know your car yet. Step 6 fixes that.

If the upload can't find the dongle, try another USB cable (some only carry power) or another port.
If no serial port appears at all, your computer needs the USB-serial driver for the dongle; see the
Freematics documentation.

## 5. Install it in the car

1. With the dongle **unplugged**, insert the SIM card.
2. Find your car's **OBD port**. It is almost always under the dashboard on the driver's side,
   sometimes behind a small cover. Search for "*your car* OBD port location" if you can't find it.
3. Switch the car on, and plug the dongle in firmly, all the way.

The car powers the dongle from here on; it needs no other cable.

**Do step 6 straight away.** Right after it's plugged in, the dongle keeps Bluetooth on and waits for
the app. Later, while the car is off, it mostly sleeps and the app may not find it.

## 6. Connect from the ABRP app

This step tells the dongle which car it's in. The app downloads the right settings for your car
from ABRP and sends them to the dongle over Bluetooth. The dongle saves them, so you only do this
once.

1. Stay in the car with it switched on, and your phone's Bluetooth on.
2. In the ABRP app, open your car and set it up with **ABRP OBD**.
3. Choose **ABRP Dongle** when the app finds it.
4. **When your phone asks to pair, accept.** If you decline, setup fails.
5. Confirm the car.

The first phone to finish setup becomes **the dongle's phone**. Other phones can't connect to it,
even when they're signed in to the same ABRP account. To change phones, see
[Troubleshooting](#troubleshooting).

Until setup finishes, **any phone nearby can do it instead**, and would then own the dongle. If setup
fails and you can't retry straight away, unplug the dongle rather than leave it in the car.

**✔ Check:** the app shows live values from the car (state of charge, for example). After this
first setup, the dongle works without your phone.

## 7. Check that it reports on its own

Turn off Bluetooth on your phone (or leave it at home) and go for a short drive.

**✔ Check:** in ABRP (on the web, or on another device), your car's state of charge and position
update while you drive. The dongle uploads about every 50 seconds, and its first connection after
being plugged in can take a few minutes, so give it a little time.

**What to expect from here on:**

| When the car is… | The dongle… |
|---|---|
| Driving | Reports about every 50 seconds over LTE-M, or over Bluetooth while your phone is connected. |
| Charging | Wakes every 3 minutes, takes a reading, uploads and goes back to sleep. On a DC fast charger it stays awake and updates about every 30 seconds while the car reports DC charging. |
| Parked and asleep | Wakes every 30 minutes or when the car moves, checks whether the car is awake, and goes straight back to sleep if it isn't. It doesn't wake the car to read it, so ABRP keeps the last value until the car wakes up. The one exception is a single read right after a charge ends, so ABRP sees the charge finish. |

If there's no signal (a parking garage, say), the dongle keeps its readings and sends them later.

## Firmware updates

Later updates don't need the USB cable: the ABRP app downloads the signed firmware and sends it to
the dongle over Bluetooth. The dongle checks the signature before installing it, and goes back to
the previous version on its own if the new one doesn't start.

During testing, firmware updates are in the app's developer tools and need an access token from the
project. Ask us for one.

If you changed the APN or the bands in step 3, update over USB instead; see the note there.

## Troubleshooting

**Your log shows where the car has been.** It records the car's GPS position along with everything
else. When we ask for it, send it to us privately; never attach it to a public issue or forum post.

**The app can't find "ABRP Dongle".**
Unplug the dongle for 10 seconds, plug it back in with the car switched on, and try again straight
away.

**Nothing shows up in ABRP at all.**
The dongle probably never got its car settings. Do step 6 again with the car switched on. Until
that setup succeeds, the dongle only waits for the app, and reads and sends nothing.

**It works over Bluetooth but not on its own.**
The LTE-M connection isn't working. Check the SIM is active, supports LTE-M, and that `CELL_APN`
and the bands are right for it (step 3). Then connect over USB, open the serial monitor, type
`RADIO`, and send us the output along with your log.

**Driving works, but charging doesn't show up.**
The dongle decides whether the car is charging from its 12 V system voltage and from the charge
current your car's profile reports. Either can be off for a car we haven't tested. Download the log
from the app after a charging session and send it to us with your car model.

**The app connects, then drops straight away, with no error.**
Your phone and the dongle disagree about whether they're paired. In your phone's Bluetooth
settings, "forget" the dongle, then connect again from the app.

**I have a new phone, reinstalled the app, or want to stop a phone using the dongle.**
The dongle's key lives only in the app on the phone that set it up, so a new phone or a reinstall can't connect until you reset it.
Unplug the dongle, plug it back in, and **within 5 minutes** connect from the phone you want to
keep. The app offers to reset the dongle's key; that phone becomes the dongle's phone and every
other one loses access. Without the app, connect over USB and type `AUTHCLEAR` in the serial
monitor, then connect the phone you're keeping straight away: until it does, any phone nearby can
take the dongle, and with it the log.

**Something else is wrong.**
The dongle keeps a log of everything it does. Download it from the app, or connect over USB and
type `DUMP` in the serial monitor, and send it to us along with your car model.

## Moving to another car

The dongle is set up for one car at a time. To move it, plug it into the new car and do step 6 again
for that car.

## Going further

- [Technical overview](technical-overview.md): how it works inside, the code layout, the settings
  and the serial console commands.
- [`DONGLE_CONTEXT.md`](../DONGLE_CONTEXT.md): the full design history and field findings.
