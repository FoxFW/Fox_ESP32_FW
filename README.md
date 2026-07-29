<div align="center">
  <h1>Fox ESP32 Firmware</h1>
  <p><em>ESP32 companion firmware for the Fox Flipper Zero apps.</em></p>
  <p>
    <a href="https://foxfw.github.io/2.0/fox-esp32-flasher.html">Flash via Browser</a>
    &nbsp;·&nbsp;
    <a href="mailto:foxcustomfirmware@gmail.com">Email Support</a>
  </p>
</div>

---

## What is Fox ESP32 Firmware?

Fox ESP32 Firmware turns a wired ESP32 into an extension of your Flipper Zero,
controlled entirely over UART at 115200 baud using a compact AT bracket-command
protocol. Once flashed, the ESP32 responds to commands from the Fox Flipper apps
— **Fox ESP32 Commander**, **Fox Chat**, **Fox Portal**, **Fox Chameleon**, and
**Fox Update Downloader** — expanding what the Flipper can do with WiFi,
Bluetooth, HTTP, and scripting.

---

## Companion Flipper Apps

None of these ship with this firmware — they're separate Flipper Zero apps in
the main [FoxFW2.0](https://github.com/FoxFW/2.0) repository, all under
`Apps → Fox` once installed.

| App | What it does |
|---|---|
| **Fox ESP32 Commander** | The main control hub — WiFi recon and attacks, BLE scanning and tag detection, the HTTP/WebSocket bridge, the FoxScript engine, and a raw Terminal |
| **Fox ESP32 Detector** | Scans every GPIO pin pair and baud rate to confirm your wiring and identify this firmware |
| **Fox ESP32 Flasher** | Flashes this firmware onto a connected board directly from the Flipper — no PC required |
| **Fox Update Downloader** | Checks GitHub for newer releases of this firmware (and FoxFW itself), downloads them over this same UART bridge, and hands off to install |
| **Fox ESP32 Terminal** | A focused live-terminal companion with per-session logs saved to the SD card |
| **Fox Chat** | Posts to and reads from a Discord channel through this firmware's internet connection |
| **Fox Portal** | Turns the board into a captive-portal access point with a configurable enquiry-form landing page |
| **Fox Chameleon** | BLE bridge between the Flipper and a Chameleon Ultra, relayed over this firmware's BLE stack |

---

## Compatible Hardware

| Board | WiFi | BLE | Notes |
|---|:---:|:---:|---|
| ESP32 Classic (WROOM-32 / DevKit) | ✓ | ✓ | Primary target |
| ESP32-WROVER | ✓ | ✓ | Extra PSRAM available |
| ESP32-CAM | ✓ | ✓ | Camera unused by firmware |
| ESP32-S2 | ✓ | — | No BLE hardware |
| ESP32-S3 | ✓ | ✓ | Uses NimBLE stack |
| ESP32-C3 | ✓ | ✓ | Uses NimBLE stack |
| ESP32-C5 | ✓ | ✓ | Uses NimBLE stack |
| ESP32-C6 | ✓ | ✓ | Uses NimBLE stack |

Connect **GPIO1 / GPIO3** (UART0 TX/RX) to your Flipper's GPIO header. Baud
rate is fixed at **115200** — no configuration required.

---

## Features

### WiFi Recon

| Command | Description |
|---|---|
| `WIFISCAN` | Active AP scan — returns SSID, BSSID, RSSI, channel, security |
| `WIFISTASCAN` | Scan for connected client stations |
| `WARDRIVE` | Wardriving scan — same as WIFISCAN, used with GPS for location tagging |
| `WIFIPINGSCAN` | ICMP ping sweep of the local /24 subnet |
| `WIFIARPSCAN` | ARP sweep — discovers live hosts and their MAC addresses |
| `WIFIMACTRACK` | Passive MAC presence tracker — logs which devices appear and disappear |
| `PORTSCAN` | TCP port scan of a target IP (up to 100 ports) |
| `SIGMON` | Signal-strength monitor for a selected AP |
| `PACKETCOUNT` | Raw packet counter per second on a selected channel |

### WiFi Passive Sniffing

| Command | Description |
|---|---|
| `SNIFF:BEACON` | Capture beacon frames (SSID / BSSID list) |
| `SNIFF:DEAUTH` | Detect deauthentication frames on air |
| `SNIFF:PROBE` | Capture probe requests — reveals what devices are searching for |
| `SNIFF:PMKID` | Capture RSN IE data from association frames for PMKID/EAPOL extraction |
| `SNIFF:SAE` | Capture WPA3 SAE (Dragonfly) commit and confirm frames |
| `SNIFF:MULTISSID` | Multi-SSID correlation — groups probes by device across SSIDs |
| `SNIFF:PCAP` | Raw PCAP capture with channel hopping, delivered as chunked hex |

### WiFi Attacks

| Command | Description |
|---|---|
| `DEAUTH` | Targeted 802.11 deauthentication burst |
| `BEACONSPAM` | Flood the 2.4 GHz band with fake AP beacons |
| `RICKROLL` | Beacon spam with a specific classic SSID list |
| `KARMA` | KARMA attack — responds to any probe request as the requested SSID |
| `EVILTWIN` | Rogue AP clone — rebroadcasts a captured AP's SSID on a different channel |
| `CSA` | Channel Switch Announcement — coerces clients onto a different channel |

> **Legal notice:** These features are intended for testing on networks and
> devices you own or have explicit authorisation to test. Unauthorised use may
> violate local telecommunications law. Always comply with your regional
> regulations.

---

### Bluetooth

| Command | Description |
|---|---|
| `BLESCAN` | BLE scan — returns nearby device name, MAC, RSSI, and raw manufacturer data |
| `BLETAGSCAN` | Tag detector — identifies Apple AirTags, Samsung SmartTags, Tile, Flock, Meta trackers and more |
| `BLESPAM:APPLE` | Apple BLE proximity notification spam |
| `BLESPAM:SAMSUNG` | Samsung Fast Pair BLE spam |
| `BLESPAM:FASTPAIR` | Google Fast Pair BLE spam |
| `BLESPAM:FLIPPER` | Flipper Zero BLE pairing advertisement spam |
| `SPOOFAT` | BLE device spoofing — impersonate a specific MAC and manufacturer data |

> BLE features require a BLE-capable board. ESP32-S2 has no BLE hardware and
> reports `HASBLE:0` — BLE commands are silently ignored on that target.

---

### HTTP & WebSocket Bridge

| Command | Description |
|---|---|
| `HTTPGET` | HTTP GET request — returns response body up to 3 KB |
| `HTTPPOST` | HTTP POST request with a JSON or plain-text body |
| `WSCONNECT` | Open a persistent WebSocket connection |
| `WSSEND` | Send a message over the active WebSocket connection |
| `WSCLOSE` | Close the WebSocket connection |

Useful for querying local APIs, hitting webhooks, or bridging the Flipper to
any HTTP-accessible service without a phone.

---

### Fox Portal

Fox Portal turns the ESP32 into a **captive portal access point**. Devices that
join the Fox Portal WiFi network are redirected to a custom web page hosted on
the ESP32. The Flipper app handles the configuration — field names, page HTML,
and SSID are all pushed to the firmware at launch. No upstream internet
connection is required.

| Feature | Detail |
|---|---|
| Custom SSID | Set the AP name from the Flipper app |
| Custom HTML | `start.html` and `finish.html` edited on SD card or via PC |
| Input fields | Up to 12 configurable named text fields |
| Submission log | Submitted responses stream back to the Flipper over UART |
| Standalone | No internet connection — runs entirely on the ESP32 soft-AP |

---

### Discord

| Command | Description |
|---|---|
| `DISCORDPOST` | Post a message to a configured Discord channel via webhook |
| `DISCORDREAD` | Read the latest N messages from a channel via the Discord API |

Token and channel ID are stored in Fox settings on the Flipper and pushed to
the firmware on connect.

---

### FoxScript Engine

FoxScript is a lightweight scripting language that runs directly on the ESP32.
Scripts are uploaded from the Flipper and executed as a single command —
useful for automating multi-step WiFi/HTTP workflows without flashing new code.

| Feature | Detail |
|---|---|
| Variables | `let x = 10` — strings, numbers, and booleans |
| Arrays | `let arr = [1, 2, 3]` with index access |
| Objects | `let obj = {key: "value"}` with dot access |
| Functions | `func myFunc(a, b) { ... }` with up to 6 call depth |
| Control flow | `if / else`, `while`, `for` loops (5000 iteration cap) |
| HTTP | `httpget(url)`, `httppost(url, body)` from within a script |
| Storage | `store(key, value)` / `load(key)` — persists values on the ESP32 |
| Print | `print(value)` — streams output back to the Flipper over UART |

---

## Optional Hardware Modules

These modules are **disabled by default** (`0` in `config.h`). Enable them by
setting the relevant flag to `1` and wiring the module to the listed pins before
flashing.

| Module | Flag | Default Pins |
|---|---|---|
| GPS (NMEA) | `FOX_HAS_GPS` | RX: 16, TX: 17 — 9600 baud |
| RFID (PN532) | `FOX_HAS_RFID` | IRQ: 25, RST: 26 — I2C/SPI |
| Sub-GHz (CC1101) | `FOX_HAS_SUBGHZ` | SCK: 18, MISO: 19, MOSI: 23, CS: 27 — 433.92 MHz default |
| IR (send/receive) | `FOX_HAS_IR` | Send: 4, Recv: 5 |

---

## Flashing

### Via Web Flasher (recommended)

**[→ Flash Fox ESP32 Firmware](https://foxfw.github.io/2.0/fox-esp32-flasher.html)** `Recommended`

1. Open the link above in **Chrome** or **Edge** on desktop.
2. Connect your ESP32 via USB.
3. Click **Connect** and select your device's COM port.
4. Click **Flash** — the installer downloads and writes the firmware automatically.
5. Connect GPIO1/3 to your Flipper and open Fox ESP32 Commander.

---

### Via Fox ESP32 Flasher (built into FoxFW)

No PC required and no files to download — the firmware for every supported board is bundled directly into FoxFW and deployed to your SD card automatically during the standard qFlipper install.

**Step 1 — Connect your ESP32**

Connect your ESP32 to the Flipper Zero GPIO header:

| ESP32 | Flipper GPIO |
|---|---|
| TX (GPIO1) | Pin 14 (RX) |
| RX (GPIO3) | Pin 13 (TX) |
| GND | GND |
| 3.3V | 3.3V |

**Step 2 — Open Fox ESP32 Flasher**

On your Flipper go to **Apps → Fox → Fox ESP32 Flasher**. The app will attempt to detect a connected ESP32 automatically. If it is not detected, check your wiring and try again.

**Step 3 — Select your board and flash**

1. Select **Flash Firmware** from the main menu.
2. Choose your board type from the list (e.g. ESP32 Classic, ESP32-S3, ESP32-C3, etc.).
3. Confirm the firmware files shown and press **Flash**.
4. Wait for the progress bar to complete — the app flashes the bootloader, partition table, and firmware in one pass.
5. Press **Done** when finished. Your ESP32 resets automatically.

Open **Fox ESP32 Commander** on your Flipper — it should detect the firmware and show the main menu.

---

### Via Arduino IDE *(Advanced)*

Requires the Arduino IDE and ESP32 board package installed on your PC.

1. Install the **ESP32 board package** via Boards Manager (`https://espressif.github.io/arduino-esp32/package_esp32_index.json`).
2. Open `Fox_ESP32_FW.ino`.
3. Select your board from **Tools → Board → ESP32 Arduino**.
4. Connect your ESP32 via USB, select the correct COM port, and click **Upload**.

---
