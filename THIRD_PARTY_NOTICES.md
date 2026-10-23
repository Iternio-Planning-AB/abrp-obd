# Third-party notices

The project's own code is under the Apache License 2.0 (see [`LICENSE`](LICENSE)). The components
below keep their own licences. Their copyright notices and licence headers stay in the files; this
list is where to find them.

## Vendored in `libraries/`

| Component | Path | Licence | Copyright |
| --- | --- | --- | --- |
| FreematicsPlus (modified) | `libraries/FreematicsPlus/`, including `utility/OBD.h` and `utility/ICM_42627.h` | BSD; the author does not specify which variant (see [stanleyhuangyc/Freematics#116](https://github.com/stanleyhuangyc/Freematics/issues/116)), so treat it as BSD-3-Clause | (C) 2012-2023 Stanley Huang, Freematics |
| TinyGPS | `libraries/FreematicsPlus/FreematicsGPS.cpp`, `.h` | LGPL-2.1-or-later, text in [`licenses/LGPL-2.1.txt`](licenses/LGPL-2.1.txt) | (C) 2008-2012 Mikal Hart |
| SparkFun ICM-20948 driver v1.0.3 | `libraries/FreematicsPlus/utility/ICM_20948_*`, `AK09916_*` | MIT, text in [`licenses/MIT-SparkFun.txt`](licenses/MIT-SparkFun.txt) | (c) 2016 SparkFun Electronics |
| Espressif BLE SPP example | `libraries/FreematicsPlus/utility/ble_spp_server.c`, `.h` | Public domain / CC0 | Espressif Systems |
| ESP32Time | `libraries/ESP32Time/` | MIT, text in [`libraries/ESP32Time/LICENSE`](libraries/ESP32Time/LICENSE) | (c) 2021 Felix Biego |
| JSON for Modern C++ | `libraries/nlohmann/` | MIT, SPDX header in each file | (c) 2013-2023 Niels Lohmann |
| mathparser | `libraries/mathparser/` | MIT, text in [`licenses/MIT-mathparser.txt`](licenses/MIT-mathparser.txt) | (c) 2023 Fatih Küçükkarakurt; extended for this project |
| zlib 1.3.2 (unmodified subset) | `libraries/zlib/` | zlib, text in [`libraries/zlib/LICENSE`](libraries/zlib/LICENSE) | (C) 1995-2026 Jean-loup Gailly and Mark Adler |

## Linked at build time, not vendored

PlatformIO fetches these; they are listed because they end up in the firmware image.

| Component | Licence |
| --- | --- |
| Arduino core for ESP32 (arduino-esp32) | LGPL-2.1 |
| ESP-IDF, including Espressif's precompiled Wi-Fi/BT/PHY libraries | Apache-2.0 |
| Mbed TLS | Apache-2.0 |

## LGPL components

TinyGPS and the Arduino core are LGPL-2.1 and statically linked. The complete source of this
project is published, so you can rebuild the firmware against a modified version of either.
