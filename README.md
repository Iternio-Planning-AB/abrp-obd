# ABRP OBD-Standalone Dongle

A small device that plugs into your EV's OBD port and sends live battery data to
[A Better Routeplanner](https://abetterrouteplanner.com/) **over its own LTE-M (low-power 4G)
connection, with no phone needed.** Whenever your car is awake (driving, charging or
preconditioning), it reports state of charge, charging and location to ABRP, even when you are
nowhere near it. While the car sleeps, ABRP shows the last value.

When the phone you set it up with is in the car with the ABRP app open, the dongle sends over
Bluetooth instead and saves its mobile data.

## What you need

| | |
|---|---|
| **Dongle** | A [Freematics ONE+](https://freematics.com/store/index.php?route=product/product&product_id=94) with the SIM7070 LTE-M cellular module |
| **SIM card** | An active data SIM that works on **LTE-M** (an ordinary 4G phone SIM often doesn't). The firmware is set up for a [Hologram](https://www.hologram.io/) SIM in Europe; another SIM or another region needs a setting changed and your own build ([how](docs/getting-started.md#3-the-sim-card)) |
| **ABRP** | An ABRP account with your car added, and the ABRP app on your phone |
| **A computer** | With a USB cable, for flashing the firmware once. Later updates come over Bluetooth from the app (Hologram/Europe builds only) |

## Will it work with my car?

| Status | Cars |
|---|---|
| ✅ **Confirmed** | Hyundai Ioniq 5, Renault Scenic E-Tech |
| 🟡 **Expected to work** | Most other EVs that ABRP supports with an OBD dongle. Their profiles only use commands the dongle supports, but nobody has tried them yet. If you do, please tell us how it went |
| ❌ **Does not work** | BMW i3, iX3 and iX: they need an addressing mode the dongle's hardware cannot do. VW e-Golf, and VW-group cars on the newer `MEB_new` profile, for a similar reason ([details](docs/technical-overview.md#vehicle-support)) |
| ❓ **Uncertain** | Mazda MX-30 |

> ⚠️ **Some cars sound their alarm** if anything talks on the OBD port while the car is locked (the
> BMW i3 is confirmed). If yours is one of them, turn on the **OBD alarm** option in the app's ABRP OBD
> settings. The dongle then only talks to the car while you drive or set it up, and it stops
> reporting charging while the car is parked.

## Get started

In short:

1. **Flash the firmware** onto the dongle from your computer.
2. **Insert the SIM card** and plug the dongle into your car's OBD port.
3. **Connect from the ABRP app** straight away, accept the pairing request, and choose your car.
   The app sends the dongle what it needs to read your car, and it remembers that from then on.
4. **Done.** Your car shows up live in ABRP, phone or no phone.

The **[Getting started guide](docs/getting-started.md)** walks through each step, how to check it
worked, and what to do if it doesn't.

## Good to know

- **One car per dongle, one phone per dongle.** The dongle is set up for the car you choose, and
  the first phone to finish setup is the only one that can connect to it.
- **It looks after your 12 V battery.** While the car is parked the dongle sleeps, and wakes every
  30 minutes (more often while charging) or when the car moves. It checks whether the car is awake
  before it asks it anything; the one exception is a single read right after a charge, so ABRP sees
  the charge end.
- **It keeps a log** on the dongle itself, which the app can download. That log is how problems get
  found. It records where the car has been, so share it privately, never in a public issue.
- **This is an active project.** The Bluetooth link is encrypted, and once set up only the dongle's
  phone can use it; signed vehicle profiles are built but not yet required.

## More

- [Getting started guide](docs/getting-started.md): the full setup, step by step
- [Technical overview](docs/technical-overview.md): how it works, the code layout and the settings
- [`DONGLE_CONTEXT.md`](DONGLE_CONTEXT.md): everything else, from design history to protocols and
  field findings. Long; best read with an AI assistant
- [`RELEASING.md`](RELEASING.md): how firmware releases are made

## License

Copyright 2024-2026 Rivian LLC. Licensed under the [Apache License 2.0](LICENSE); see also
[`NOTICE`](NOTICE). Vendored and linked third-party code keeps its own licence; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

The license covers the code, not the names: Apache-2.0 grants no rights to "ABRP", "A Better
Routeplanner" or "Iternio" (section 6), so forks must not use them to name or brand their builds.
