# Battery monitor sensors (Wattcycle, Victron and Bluetti)

This branch adds MeshCore sensor builds that read battery data over Bluetooth and report it in the node's telemetry, with a high priority alert when charge gets low:

* **Wattcycle sensor**: connects to Wattcycle LiFePO4 batteries that have Bluetooth (TDT smart BMS) and reads them.
* **Victron sensor**: listens for the encrypted "Instant Readout" broadcasts from Victron battery monitors such as the SmartShunt. It never connects to them.
* **Bluetti sensor**: connects to Bluetti power stations (tested with the Elite 30 V2: EL30V2 and PR30V2) and reads battery %, time remaining (EL30V2 only), DC (solar) input, AC input, DC output and AC output.

> **Important: your battery details are built into the firmware.** Edit `platformio.ini` with your own battery names (for Victron, your keys; for Bluetti, your serial numbers) **before** you build and flash. The settings below are the author's own devices, so a node flashed without editing them will not find yours. If you change a setting later, rebuild and flash again.

## 1. Pick your build

| Board | File to edit | Wattcycle build | Victron build | Bluetti build |
|---|---|---|---|---|
| Heltec V3 | `variants/heltec_v3/platformio.ini` | `Heltec_v3_wattcycle_sensor` | `Heltec_v3_victron_sensor` | `Heltec_v3_bluetti_sensor` |
| Heltec V4 (screenless) | `variants/heltec_v4/platformio.ini` | `heltec_v4_wattcycle_sensor` | `heltec_v4_victron_sensor` | `heltec_v4_bluetti_sensor` |
| Seeed XIAO ESP32-S3 + Wio-SX1262 | `variants/xiao_s3_wio/platformio.ini` | `Xiao_S3_WIO_wattcycle_sensor` | `Xiao_S3_WIO_victron_sensor` | `Xiao_S3_WIO_bluetti_sensor` |

Each build has its own section in the file, starting with `[env:<build name>]`. Only edit the lines inside the section for the build you are going to flash.

## 2a. Wattcycle: find your battery's Bluetooth name

Each Wattcycle battery has a Bluetooth name such as `XDZN_001_E4E7`. Either:

* open the **WattCycle** app: the name is shown at the top of the battery's card under **Connected Devices**, or
* scan from a computer with [aiobmsble](https://github.com/patman15/aiobmsble). Batteries it can read are listed with `Found matching BMS type: TDT smart BMS`:

  ```
  python3 -m venv ~/bms-test
  source ~/bms-test/bin/activate
  pip install aiobmsble aiooui
  aiobmsble -v
  deactivate
  ```

Close the WattCycle app before the node (or the scan) tries to read the battery.

## 2b. Victron: turn on Instant Readout and copy the key

For each Victron device:

1. In **VictronConnect**, open the device, tap the **gear** icon, open the menu and choose **Product info**.
2. Scroll down to **Instant readout via Bluetooth** and turn it on.
3. Tap **Show** next to **Instant readout details** and copy the **encryption key** (32 characters, 0-9 and a-f).
4. Note the device's Bluetooth name: its product name followed by its serial number, for example `SmartShunt HQ2301RCTU4`.

Treat the key like a password: anyone who has it can decode that device's readings. Don't commit it to a public repository.

## 2c. Bluetti: find each unit's serial number

The node finds each unit by matching the end of its Bluetooth name against what you list, so list the serial number (the PR30V2 tested advertises as `PR30V22603110572960`: model, then serial number). Read the serial number from a computer with [bluetti-bt-lib](https://pypi.org/project/bluetti-bt-lib/):

```
python3 -m venv ~/bluetti-venv
source ~/bluetti-venv/bin/activate
pip install bluetti-bt-lib
bluetti-scan
bluetti-read -m <address from bluetti-scan> -t <model from bluetti-scan> -e 1
deactivate
```

`bluetti-scan` prints a model and an address for each unit, such as `['PR30V2', '3DEA681C-...']`: pass only the address to `-m`. The `FieldName.DEVICE_SN` line of `bluetti-read` is the serial number. These units need the encrypted connection (`-e 1`); the node always uses it.

Close the BLUETTI app before the node (or the read) connects to a unit.

## 3. Edit `platformio.ini` (before flashing)

### Wattcycle builds

| Line | What to put |
|---|---|
| `-D BMS_NAMES='"XDZN_001_E4E7"'` | Your battery's name from step 2a. For more than one battery, put them all inside the **same** quotes, separated by commas, up to 4: `'"XDZN_001_AAAA,XDZN_001_BBBB"'` |
| `-D BMS_LOW_ALERT_PCT=20` | Charge % at or below which the node sends a high priority alert |
| `-D BMS_POLL_INTERVAL_SECS=240` | Seconds between reads. Readings older than 300 seconds are left out of telemetry and skipped by the alert, so keep this well under 300 (or also add `-D BMS_STALE_SECS=<seconds>`) |
| `-D BMS_BLE_TX_POWER=ESP_PWR_LVL_P3` | Optional Bluetooth transmit power, from `ESP_PWR_LVL_N24` (-24 dBm) to `ESP_PWR_LVL_P21` (+21 dBm, the default when this line is absent) |
| `-D BMS_DEBUG=1` | Status lines on the serial monitor; set to `0` to turn them off |

### Victron builds

| Line | What to put |
|---|---|
| `-D VTR_DEVICES='"SmartShunt HQ2301RCTU4:<key>"'` | `name:key` for each device, from step 2b. For more than one device, separate entries with commas inside the **same** quotes, up to 4. If you write the entry as `:<key>` (nothing before the colon), the node accepts any battery monitor whose broadcast decodes with that key |
| `-D VTR_LOW_ALERT_PCT=20` | Charge % at or below which the node sends a high priority alert |
| `-D VTR_SCAN_SECS=10` | How long each listening cycle scans for |
| `-D VTR_SCAN_INTERVAL_SECS=240` | Seconds between listening cycles. Readings older than 300 seconds are left out of telemetry, so keep the gap well under 300 |
| `-D VTR_DEBUG=1` | Status lines on the serial monitor; set to `0` to turn them off |

### Bluetti builds

| Line | What to put |
|---|---|
| `-D BLUETTI_UNITS='"2603110572960,2534001795459,2523000091359"'` | Each unit's serial number from step 2c, separated by commas inside the **same** quotes, up to 4. The full Bluetooth name also works |
| `-D BLUETTI_LOW_ALERT_PCT=10` | Charge % at or below which the node sends a high priority alert |
| `-D BLUETTI_POLL_INTERVAL_SECS=240` | Seconds between reads. Readings older than 600 seconds are left out of telemetry and skipped by the alert (change with `-D BLUETTI_STALE_SECS=<seconds>`) |
| `-D BLUETTI_BLE_TX_POWER=ESP_PWR_LVL_P3` | Optional Bluetooth transmit power, from `ESP_PWR_LVL_N24` (-24 dBm) to `ESP_PWR_LVL_P21` (+21 dBm, the default when this line is absent) |
| `-D BLUETTI_DEBUG=1` | Status lines on the serial monitor; set to `0` to turn them off |

### All builds

* `-D ADMIN_PASSWORD='"password"'`: change this, or change it after flashing with the `password` command.
* `-D ADVERT_NAME='"..."'`: the node's name on the mesh (also changeable later with `set name`).

## 4. Build and flash

With the board plugged in over USB:

```
pio run -e <build name> -t upload
```

For a merged image (bootloader, partitions and app in one file, for a fresh install):

```
pio run -e <build name> -t mergebin
```

which writes `.pio/build/<build name>/firmware-merged.bin`.

## 5. First-time setup (serial monitor)

Run `pio device monitor -b 115200`, then:

1. `set radio <freq>,<bw>,<sf>,<cr>` with your mesh's settings (`get radio` on an existing node shows them), then `reboot`.
2. `password <new password>` if you didn't change it in `platformio.ini`.
3. `setperm <your companion's 64-character public key> 131` so your companion can log in as admin and receive the high priority alerts.

## 6. Check it

* With debug on, startup lines starting `BMS:` (Wattcycle), `VTR:` (Victron) or `BLUETTI:` (Bluetti) list the devices the node is looking for, then each read.
* In the MeshCore app, request telemetry from the node. Channel 1 is the node itself.
* Wattcycle and Victron: the first battery or device in your list is **channel 10**, the second **channel 11**, and so on. A channel only appears once that device has a reading less than 300 seconds old.
* Bluetti: each unit gets five channels. The first unit in your list uses 10 to 14, the second 20 to 24, the third 30 to 34, the fourth 40 to 44. They only appear once that unit has a reading less than 600 seconds old.

  | Channel (first unit) | Reading |
  |---|---|
  | 10 | Battery %, and time remaining in minutes (EL30V2 only, shown as a plain number) |
  | 11 | DC input watts (solar) |
  | 12 | AC input watts |
  | 13 | DC output watts |
  | 14 | AC output watts |

---

## About MeshCore

MeshCore is a lightweight, portable C++ library that enables multi-hop packet routing for embedded projects using LoRa and other packet radios. It is designed for developers who want to create resilient, decentralized communication networks that work without the internet.

## 🔍 What is MeshCore?

MeshCore now supports a range of LoRa devices, allowing for easy flashing without the need to compile firmware manually. Users can flash a pre-built binary using tools like Adafruit ESPTool and interact with the network through a serial console.
MeshCore provides the ability to create wireless mesh networks, similar to Meshtastic and Reticulum but with a focus on lightweight multi-hop packet routing for embedded projects. Unlike Meshtastic, which is tailored for casual LoRa communication, or Reticulum, which offers advanced networking, MeshCore balances simplicity with scalability, making it ideal for custom embedded solutions, where devices (nodes) can communicate over long distances by relaying messages through intermediate nodes. This is especially useful in off-grid, emergency, or tactical situations where traditional communication infrastructure is unavailable.

## ⚡ Key Features

* Multi-Hop Packet Routing
  * Devices can forward messages across multiple nodes, extending range beyond a single radio's reach.
  * Supports up to a configurable number of hops to balance network efficiency and prevent excessive traffic.
  * Nodes use fixed roles where "Companion" nodes are not repeating messages at all to prevent adverse routing paths from being used.
* Supports LoRa Radios – Works with Heltec, RAK Wireless, and other LoRa-based hardware.
* Decentralized & Resilient – No central server or internet required; the network is self-healing.
* Low Power Consumption – Ideal for battery-powered or solar-powered devices.
* Simple to Deploy – Pre-built example applications make it easy to get started.

## 🎯 What Can You Use MeshCore For?

* Off-Grid Communication: Stay connected even in remote areas.
* Emergency Response & Disaster Recovery: Set up instant networks where infrastructure is down.
* Outdoor Activities: Hiking, camping, and adventure racing communication.
* Tactical & Security Applications: Military, law enforcement, and private security use cases.
* IoT & Sensor Networks: Collect data from remote sensors and relay it back to a central location.

## 🚀 How to Get Started

- Watch the [MeshCore QuickStart Playlist](https://www.youtube.com/watch?v=iaFltojJrAc&list=PLshzThxhw4O4WU_iZo3NmNZOv6KMrUuF9) by The Comms Channel
- Watch the [MeshCore Technical Presentation](https://www.youtube.com/watch?v=OwmkVkZQTf4) by Liam Cottle.
- Read through our [Frequently Asked Questions](./docs/faq.md) and [Documentation](https://docs.meshcore.io).
- Flash the MeshCore firmware on a supported device.
- Connect with a supported client.

For developers:

- Install [PlatformIO](https://docs.platformio.org) in [Visual Studio Code](https://code.visualstudio.com).
- Clone and open the MeshCore repository in Visual Studio Code.
- See the example applications you can modify and run:
  - [Companion Radio](./examples/companion_radio) - For use with an external chat app, over BLE, USB or Wi-Fi.
  - [KISS Modem](./examples/kiss_modem) - Serial KISS protocol bridge for host applications. ([protocol docs](./docs/kiss_modem_protocol.md))
  - [Simple Repeater](./examples/simple_repeater) - Extends network coverage by relaying messages.
  - [Simple Room Server](./examples/simple_room_server) - A simple BBS server for shared Posts.
  - [Simple Secure Chat](./examples/simple_secure_chat) - Secure terminal based text communication between devices.
  - [Simple Sensor](./examples/simple_sensor) - Remote sensor node with telemetry and alerting.

The Simple Secure Chat example can be interacted with through the Serial Monitor in Visual Studio Code, or with a Serial USB Terminal on Android.

## ⚡️ MeshCore Flasher

We have prebuilt firmware ready to flash on supported devices.

- Launch https://meshcore.io/flasher
- Select a supported device
- Flash one of the firmware types:
  - Companion, Repeater or Room Server
- Once flashing is complete, you can connect with one of the MeshCore clients below.

## 📱 MeshCore Clients

**Companion Firmware**

The companion firmware can be connected to via BLE, USB or Wi-Fi depending on the firmware type you flashed.

- Web: https://app.meshcore.nz
- Android: https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android
- iOS: https://apps.apple.com/us/app/meshcore/id6742354151?platform=iphone
- NodeJS: https://github.com/liamcottle/meshcore.js
- Python: https://github.com/fdlamotte/meshcore-cli

**Repeater and Room Server Firmware**

The repeater and room server firmware can be set up via USB in the web config tool.

- https://config.meshcore.io

They can also be managed via LoRa in the mobile app by using the Remote Management feature.

## 🛠 Hardware Compatibility

MeshCore is designed for devices listed in the [MeshCore Flasher](https://meshcore.io/flasher)

## 📜 License

MeshCore is open-source software released under the MIT License. You are free to use, modify, and distribute it for personal and commercial projects.

## Contributing

Please submit PR's using 'dev' as the base branch!
For minor changes just submit your PR and we'll try to review it, but for anything more 'impactful' please open an Issue first and start a discussion. It is better to sound out what it is you want to achieve first, and try to come to a consensus on what the best approach is, especially when it impacts the structure or architecture of this codebase.

Here are some general principles you should try to adhere to:
* Keep it simple. Please, don't think like a high-level lang programmer. Think embedded, and keep code concise, without any unnecessary layers.
* No dynamic memory allocation, except during setup/begin functions.
* Use the same brace and indenting style that's in the core source modules. (A .clang-format is probably going to be added soon, but please do NOT retroactively re-format existing code. This just creates unnecessary diffs that make finding problems harder)

Help us prioritize! Please react with thumbs-up to issues/PRs you care about most. We look at reaction counts when planning work.

### Running unit tests

To run unit tests, run the following command:

```bash
pio test --environment native --verbose
```

## Road-Map / To-Do

There are a number of fairly major features in the pipeline, with no particular time-frames attached yet. In very rough chronological order:
- [X] Companion radio: UI redesign
- [X] Repeater + Room Server: add ACL's (like Sensor Node has)
- [X] Standardise Bridge mode for repeaters
- [ ] Repeater/Bridge: Standardise the Transport Codes for zoning/filtering
- [X] Core + Repeater: enhanced zero-hop neighbour discovery
- [ ] Core: round-trip manual path support
- [ ] Companion + Apps: support for multiple sub-meshes (and 'off-grid' client repeat mode)
- [ ] Core + Apps: support for LZW message compression
- [ ] Core: dynamic CR (Coding Rate) for weak vs strong hops
- [ ] Core: new framework for hosting multiple virtual nodes on one physical device
- [ ] V2 protocol spec: discussion and consensus around V2 packet protocol, including path hashes, new encryption specs, etc

## 📞 Get Support

- Report bugs and request features on the [GitHub Issues](https://github.com/meshcore-dev/meshcore/issues) page.
- Find additional guides and components on [my site](https://buymeacoffee.com/ripplebiz).
- Join [MeshCore Discord](https://meshcore.gg) to chat with the developers and get help from the community.
