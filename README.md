<p align="center">
  <h1 align="center">CrowPanel Advance 5" (DIS02050A v1.1 & V1.2)<br>Dual-Boot LoRa Mesh Firmware</h1>
  <p align="center">
    Run <b>MeshCore</b> or a custom <b>Meshtastic</b> UI on your CrowPanel - switch at boot, no reflashing.
  </p>
  <p align="center">
    <img src="https://img.shields.io/badge/ESP32--S3-LoRa_Mesh-blue?style=flat-square" alt="ESP32-S3">
    <img src="https://img.shields.io/badge/display-5%22_800x480-green?style=flat-square" alt="Display">
    <img src="https://img.shields.io/badge/radio-SX1262-orange?style=flat-square" alt="SX1262">
    <img src="https://img.shields.io/badge/license-GPL--3.0-red?style=flat-square" alt="License">
  </p>
</p>

---

## Hardware

<p align="center">
  <a href="https://www.elecrow.com/crowpanel-advance-5-0-hmi-esp32-ai-display-800x480-ips-artificial-intelligent-touch-screen.html">
    <img src="docs/images/crowpanel-front.jpg" width="380" alt="CrowPanel Advance 5.0 Front">
    &nbsp;&nbsp;
    <img src="docs/images/crowpanel-back.jpg" width="380" alt="CrowPanel Advance 5.0 PCB">
  </a>
</p>

<p align="center">
  <b>Elecrow CrowPanel Advance 5.0" HMI</b> — <a href="https://www.elecrow.com/crowpanel-advance-5-0-hmi-esp32-ai-display-800x480-ips-artificial-intelligent-touch-screen.html">Product Page</a>
</p>

| Component | Specification |
|-----------|--------------|
| **Board** | Elecrow CrowPanel Advance 5.0" (DIS02050A v1.1) |
| **MCU** | ESP32-S3 (8MB Flash, PSRAM) |
| **Display** | 5" 800x480 IPS, capacitive touch (GT911) |
| **Radio** | SX1262 LoRa transceiver |
| **Connectivity** | WiFi + Bluetooth (built-in) |

---

## Screenshots

### Dual boot selector with OTA support

<p align="center">
  <img src="docs/images/bootloader-ota.jpg" width="280">
</p>

### Meshtastic

| Home Screen | Settings (top) | Settings (bottom) | GPS Location |
|:---:|:---:|:---:|:---:|
| <img src="docs/images/meshtastic-homescreen.jpg" width="160"> | <img src="docs/images/meshtastic-settings-1.jpg" width="160"> | <img src="docs/images/meshtastic-settings-2.jpg" width="160"> | <img src="docs/images/gps-location.png" width="160"> |

### MeshCore

| Chat List | Chat & Keyboard | Settings (top) | Settings (bottom) |
|:---:|:---:|:---:|:---:|
| <img src="docs/images/chats.jpg" width="160"> | <img src="docs/images/chat.jpg" width="160"> | <img src="docs/images/settings1.jpg" width="160"> | <img src="docs/images/settings2.jpg" width="160"> |

---

## Overview

This project turns the **CrowPanel Advance 5"** into a standalone LoRa mesh communicator with a full touchscreen UI. A boot selector lets you choose which firmware to run at startup, connect to WiFi, and update the app firmwares from GitHub releases.

| Firmware | Description |
|----------|-------------|
| **MeshCore** | Feature-rich mesh chat with a dark-themed LVGL touchscreen UI built entirely from scratch, with original features and WiFi functionality (Telegram bridge, web dashboard, translation, emoji support). |
| **Meshtastic** | Custom CrowPanel-focused Meshtastic UI with touch chat screens, nodes, settings, private chat actions, and CrowPanel display/backlight support. |
| **Boot Selector** | Touchscreen dual-boot menu with WiFi setup and OTA app firmware updates from GitHub releases. Remembers your last choice. |

---

## MeshCore Features

| Feature | Details |
|---------|---------|
| **Custom UI** | Built from scratch with LVGL 8.3 — dark theme, portrait & landscape, Greek/English keyboards |
| **Private Messages** | Per-message delivery tracking with automatic retries and resend button on failure |
| **Channels** | Group messaging with receipt confirmation |
| **Emoji Support** | Monochrome emoji keyboard (2 pages) + rendering of incoming emojis from phones |
| **Translation** | Auto-translate or long-press to translate messages (Google Translate, English/Greek/Dutch/German/Italian/French) |
| **Web Interface** | Full web dashboard accessible from any browser — view contacts, channels, messages, send/receive over WiFi |
| **Telegram Bridge** | Channels to group topics, PMs to private bot chat, bidirectional messaging |
| **Gesture Navigation** | Swipe left to go back, swipe up from bottom edge to go home |
| **Signal Info** | Hop count and SNR displayed on each message, persisted in chat history |
| **Contacts & Repeaters** | Full contact and repeater management with signal routing and path discovery |
| **WiFi + NTP** | Time sync and connectivity for all bridge and web features |
| **Persistent Settings** | TX power, language, auto-translate, and all preferences saved across reboots |

---

## Meshtastic Features

| Feature | Details |
|---------|---------|
| **Custom UI** | CrowPanel-focused Meshtastic interface with chat list, message view, nodes, and settings screens |
| **Touch Keyboard** | On-screen keyboard with a MeshCore-style layout tuned for the 5" display |
| **Custom Channels** | Add or join Meshtastic channels directly from the device — tap the "+" on the Chats tab, enter a name, and leave the key blank to create a new channel or paste a base64 PSK to join an existing one |
| **MQTT Controls** | Built-in MQTT settings popup (server, credentials, root topic, TLS/JSON/encryption) plus per-channel bridge toggles, including primary LongFast uplink/downlink |
| **Private Chats** | Long-press private chats to delete local chat history with confirmation |
| **Security Tools** | Regenerate private keys directly from settings |
| **Radio Defaults** | TX power defaults to 20 dBm, with 22 dBm still available as a manual option |
| **CrowPanel Display** | Dedicated LVGL display setup, backlight handling, and safer display buffer fallback |

---

## OTA Bootloader

| Feature | Details |
|---------|---------|
| **Dual Boot** | Choose MeshCore or Meshtastic at startup from a touchscreen selector |
| **WiFi Setup** | Configure WiFi directly from the selector before updating |
| **OTA Updates** | Downloads the latest `meshcore.bin` and `meshtastic.bin` from GitHub releases |
| **Protected Core** | OTA updates replace only the app firmware slots; bootloader, selector, and partitions are not updated |
| **Version Display** | Shows detected MeshCore and Meshtastic firmware versions on the boot buttons |

---

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- Python 3 (included with PlatformIO)
- USB-C cable

### Hardware Setup

> **Important: Antenna Pigtail Cable**
>
> Route the LoRa antenna pigtail cable **outside the board** (not folded over the PCB). Keeping the cable away from the electronics significantly reduces floor noise and improves radio performance.
>
> After flashing, use the **Floor Noise** function in the settings to tune and verify your noise level. A lower floor noise means better receive sensitivity and longer range.

### Build & Flash

Clone the repo and use `flash_all.py` to build all three firmwares and flash them:

```bash
git clone https://github.com/kgiannadakis/CrowPanel-DIS02050A.git
cd CrowPanel-DIS02050A
python flash_all.py <PORT>
```

Replace `<PORT>` with your serial port (e.g. `COM20` on Windows, `/dev/ttyUSB0` on Linux, `/dev/cu.usbserial` on macOS).

The dual-boot system requires the specific `partitions.bin` included in the repo root. The bootloader is automatically picked from the Meshtastic build output.

> **Note:** The build and flash process will take several minutes — be patient! The first boot after installation will also be longer than usual.

### Flash Pre-Built Binaries

If you don't want to build from source, download the pre-built binaries from the [latest release](https://github.com/kgiannadakis/CrowPanel-DIS02050A/releases/latest) and flash them directly:

```bash
python -m esptool --chip esp32s3 --port <PORT> --baud 921600 write_flash \
  0x0000   bootloader.bin \
  0x8000   partitions.bin \
  0x10000  selector.bin \
  0x110000 meshcore.bin \
  0x660000 meshtastic.bin
```

Replace `<PORT>` with your serial port (e.g. `COM20` on Windows, `/dev/ttyUSB0` on Linux, `/dev/cu.usbserial` on macOS).

---

## Web Interface

MeshCore includes a built-in web dashboard for controlling your node from a PC or phone browser:

1. Enable WiFi on the CrowPanel (**Web Apps** screen)
2. Enable the **Web Dashboard** toggle
3. Open `http://<device-ip>` in any browser on the same network

From the web interface you can:
- View all contacts, channels, and repeaters
- Read message history
- Send private messages and channel messages
- Monitor device status (uptime, signal, battery)
- Delete repeaters

---

## Translation

MeshCore supports automatic message translation via Google Translate:

1. Go to **Web Apps** screen on the CrowPanel
2. In the **Translation** section, select your target language
3. Enable **Auto-Translate** to translate all incoming messages automatically
4. Or leave it off and **long-press any incoming message** to translate on demand

Supported target languages: English, Greek, Dutch, German, Italian, French.

Translations appear below the original message in smaller, lighter text inside the same bubble.

---

## Telegram Bridge

Bridge your mesh conversations to Telegram with organized threading:

**Setup:**
1. Create a bot via [@BotFather](https://t.me/BotFather)
2. Create a Telegram group with Topics enabled, add the bot as admin
3. Enter bot token and group chat ID on the CrowPanel (**Web Apps** screen)
4. Send `/start` to the bot in a private message to link PMs

**How it works:**
- Each mesh **channel** gets its own topic thread in the Telegram group
- **PMs** go to your private chat with the bot (only you can see them)
- Send from Telegram: `/pm ContactName message` or `/ch ChannelName message`

---

## Repository Structure

```
CrowPanel-DIS02050A/
├── meshcore/        MeshCore firmware (PlatformIO project)
├── meshtastic/      Meshtastic firmware (PlatformIO project)
├── selector/        Boot selector firmware (PlatformIO project)
├── partitions.bin   Dual-boot partition table (pre-built)
├── flash_all.py     Build & flash script
└── LICENSE          GPL-3.0
```

---

## Acknowledgments

- [Meshtastic](https://meshtastic.org/) — Open-source LoRa mesh networking
- [MeshCore](https://github.com/meshcore-dev/MeshCore) — LoRa mesh chat framework
- [Elecrow](https://www.elecrow.com/) — CrowPanel hardware
- [LVGL](https://lvgl.io/) — Embedded graphics library
- [Noto Emoji](https://fonts.google.com/noto/specimen/Noto+Emoji) — Monochrome emoji font

---

## License

This project is licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).

---

## Disclaimer

I am not a professional programmer. This project is the result of a lot of hard work, manual patches, use of AI tools, and trial and error. Expect some functions not to be perfect. This is maintained in my free time, so updates may be infrequent.

---

## Contributing

Contributions are welcome! Feel free to open issues or pull requests.

If you're adapting this for a different CrowPanel model, the key files to modify are the display driver, pin definitions, and partition table.
