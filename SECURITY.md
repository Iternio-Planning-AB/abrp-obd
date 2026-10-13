# Security policy

This dongle sits on a car's OBD port. A client that can write to the dongle can write to the car's
diagnostic bus, so security problems here can affect a vehicle, not only data.

## Reporting a vulnerability

**Do not open a public issue.** Report it privately through GitHub: open the repository's
**Security** tab and choose **Report a vulnerability**. Only the maintainers can see the report.

Please include what you found, the firmware version, and how to reproduce it. We will acknowledge
the report, keep you updated while we work on a fix, and credit you in the advisory if you want.

In scope, among others:

- Bluetooth pairing, authentication and access control (anything that lets an unpaired phone or
  another device talk to the dongle)
- Firmware updates (bypassing signature checks, downgrades, getting unsigned code to run)
- Anything that lets a client send commands to the car, or read or write the car's ECUs beyond what
  a vehicle profile asks for
- Leaking the dongle's log, the location history in it, or the ABRP vehicle token
- The cloud upload path and the release signing tooling in this repository

Problems in the ABRP app or service are welcome too; we will pass them on.

## Logs and location data

The dongle's flash log records where the car has been. Never post a log, a `DUMP` output or a
screenshot of one publicly, not in an issue, a pull request or a forum. If a maintainer needs it,
they will ask for it privately.

## Supported versions

Only the latest release gets security fixes. Update to it before reporting, if you can.
