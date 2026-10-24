# WLED Modbus TCP and EtherNet/IP PCCC usermods

Two [WLED](https://github.com/wled/WLED) usermods for controlling WLED from
industrial controllers:

* **Modbus_TCP** adds a Modbus TCP server (port 502), so a PLC, SCADA system,
  Node-RED, Home Assistant or any Modbus client can control power, brightness,
  presets, effects, palettes, speed/intensity and RGBW colors per segment.
* **EIP_PCCC** lets an Allen-Bradley PLC (ControlLogix/CompactLogix,
  MicroLogix, SLC 5/05, PLC-5/E) do the same with ordinary MSG instructions
  (PLC-5/SLC typed read and write over EtherNet/IP, port 44818). WLED state
  appears as data-table files: N7 control, N8 status, N10+ segments, B3 bits.
  See [usermods/EIP_PCCC/readme.md](usermods/EIP_PCCC/readme.md).

Built and tested against **WLED v16.0.1** on an EternaGlo Plus (ESP32 with
WT32-ETH01 Ethernet) driving a 60-LED WS2814 RGBW strip.

## Modbus quick reference

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

## EtherNet/IP PCCC quick reference

| Address | What it does |
|---|---|
| N7:0 | Power (0/1) |
| N7:1 | Master brightness (1–255 sets the level without changing power, 0 = off) |
| N7:2 | Preset (applied when the written value changes) |
| N10:2–5 | Segment 0 effect, speed, intensity, palette |
| N10:10–13 | Segment 0 primary color R, G, B, W |
| B3:0/0 | Power |
| N8:0–13 | LED count, segment/effect/palette counts, FPS, current draw, RSSI, uptime (read only) |

From a Logix controller use a *PLC5 Typed Write* (or *SLC Typed Write*) MSG
from an INT tag to `N7:0`, path `<Ethernet port>, 2, <WLED IP>`.

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
cp -r usermods/EIP_PCCC WLED/usermods/     # for the EtherNet/IP PCCC usermod
cd WLED && pio run -e esp32_eth_modbus     # or esp32dev_modbus for WiFi only
                                           # esp32_eth_pccc / esp32dev_pccc for PCCC
```

The output is in `WLED/build_output/release/`. Add `audioreactive` to
`custom_usermods` in `platformio_override.ini` to keep sound-reactive effects.
Both usermods can go in one build: `custom_usermods = Modbus_TCP EIP_PCCC`.
GitHub Actions builds all four firmware files on every push (see the
workflow's `firmware` artifact).

## Tests and tools

* `make -C test` compiles each usermod against a small WLED stub and checks the
  protocol handling: Modbus reads, writes, exceptions and cyclic-write behavior;
  EtherNet/IP sessions, unconnected and connected messaging, every PCCC command
  and error, plus a fuzz pass over malformed frames.
* `tools/modbus_test.py <ip>` (needs `pip install pymodbus`) cycles the strip
  through red, green, blue and white and switches to an effect.
* `tools/pccc_test.py <ip> [--connected]` (standard library only) does the same
  over EtherNet/IP PCCC, the way a PLC MSG instruction would.
* `tools/diag.py <ip>` (standard library only) logs how WLED responds to
  power-off over Modbus compared with the JSON API.

## Licence

EUPL-1.2, the same licence as WLED.
