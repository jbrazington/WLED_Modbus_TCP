# WLED Modbus TCP usermod

A [WLED](https://github.com/wled/WLED) usermod that adds a Modbus TCP server
(port 502), so a PLC, SCADA system, Node-RED, Home Assistant or any Modbus
client can control power, brightness, presets, effects, palettes,
speed/intensity and RGBW colors per segment.

Built and tested against **WLED v16.0.1** on an EternaGlo Plus (ESP32 with
WT32-ETH01 Ethernet) driving a 60-LED WS2814 RGBW strip.

## Quick reference

| Register / coil | What it does |
|---|---|
| Holding 0 | Power (0/1) |
| Holding 1 | Master brightness (1–255 sets the level without changing power, 0 = off) |
| Holding 2 | Preset (applied when the written value changes) |
| Holding 102–105 | Segment 0 effect, speed, intensity, palette |
| Holding 110–113 | Segment 0 primary color R, G, B, W |
| Coil 0 | Power |
| Input 0–13 | LED count, segment/effect/palette counts, FPS, current draw, RSSI, uptime |

Writing a value that is already set is a no-op, so clients that write their
whole register table every cycle (PLCs, ModMaster) are safe.

Full map, function codes, exceptions and settings:
[usermods/Modbus_TCP/readme.md](usermods/Modbus_TCP/readme.md).

## Install prebuilt firmware

`firmware/` has two WLED 16.0.1 builds for classic ESP32 (4 MB), without the
audio reactive usermod:

* `WLED_16.0.1_ESP32_Ethernet_ModbusTCP.bin`: with Ethernet (release name `ESP32_Ethernet`)
* `WLED_16.0.1_ESP32_ModbusTCP.bin`: WiFi only (release name `ESP32`)

Flash from WLED's Config → Security & Updates → Manual OTA update.
Step-by-step instructions, including Ethernet and WS2814 settings, are in
[SETUP.md](SETUP.md).

## Build it yourself

```sh
git clone -b v16.0.1 https://github.com/wled/WLED.git
cp -r usermods/Modbus_TCP WLED/usermods/
cp platformio_override.ini WLED/
cd WLED && pio run -e esp32_eth_modbus     # or esp32dev_modbus for WiFi only
```

The output is in `WLED/build_output/release/`. Add `audioreactive` to
`custom_usermods` in `platformio_override.ini` to keep sound-reactive effects.
GitHub Actions builds both firmware files on every push (see the workflow's
`firmware` artifact).

## Tests and tools

* `make -C test` compiles the usermod against a small WLED stub and checks the
  Modbus protocol handling (reads, writes, exceptions, cyclic-write behavior).
* `tools/modbus_test.py <ip>` (needs `pip install pymodbus`) cycles the strip
  through red, green, blue and white and switches to an effect.
* `tools/diag.py <ip>` (standard library only) logs how WLED responds to
  power-off over Modbus compared with the JSON API.

## Licence

EUPL-1.2, the same licence as WLED.
