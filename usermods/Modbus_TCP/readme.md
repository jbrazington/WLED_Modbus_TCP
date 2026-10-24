# Modbus TCP usermod

Adds a Modbus TCP server (default port 502) to WLED so a PLC, SCADA system,
Node-RED, Home Assistant or any Modbus client can control power, brightness,
presets, effects, palettes, speed/intensity and RGBW colors per segment.

* Function codes: 01, 02, 03, 04, 05, 06, 15 (0x0F), 16 (0x10)
* Up to 4 simultaneous clients (`-D MODBUS_TCP_MAX_CLIENTS=n` to change)
* The server is polled from the WLED main loop, so it is safe alongside the
  web UI, JSON API, MQTT and sync. Changes made over Modbus show up in the UI
  immediately and vice versa.
* Built and tested against WLED **v16.0.1**.

All addresses below are **0-based protocol addresses**. If your client uses
1-based "Modicon" numbering, add 1 (holding register 1 = `40002`, coil 0 = `00001`).
All values are unsigned 16-bit unless noted.

## Holding registers (FC 03 read, FC 06 / 16 write)

### Global (0–99)

| Addr | Name | Range | Notes |
|---|---|---|---|
| 0 | Power | 0/1 | 1 = on (restores last brightness), 0 = off |
| 1 | Master brightness | 0–255 | 1–255 sets the level and does **not** change power (while off it sets the level used at the next power on). 0 turns off. Reads the level even while off |
| 2 | Preset | 0–250 | Applies a preset when the written value **changes** (writing the same number again does nothing, so cyclic writers are safe). Write 0, then the number, to re-apply the same preset. Reads the active preset (0 = none). A preset also sets power if it was saved with it |
| 3 | Transition time | 0–65535 ms | Global transition duration |
| 4 | Nightlight duration | 1–255 min | |
| 5 | Nightlight target brightness | 0–255 | |
| 6 | Main segment | 0..segments-1 | |

Unused addresses in this block read as 0 and ignore writes.

Writing a register or coil with the value it already has does nothing, so a
PLC or a tool like ModMaster can write its whole table every cycle without
restarting effects or re-applying presets.

### Per segment

Segment **N** (0–31) uses the block starting at **100 × (N + 1)**:
segment 0 = 100–199, segment 1 = 200–299, … segment 31 = 3200–3299.
With a single strip you will normally only use segment 0 (registers 100–124).

| Offset | Seg 0 addr | Name | Range |
|---|---|---|---|
| +0 | 100 | Segment on | 0/1 |
| +1 | 101 | Segment brightness (opacity) | 0–255 (0 turns segment off, 1–255 only sets the level) |
| +2 | 102 | Effect ID | 0..effect count-1 (input reg 2) |
| +3 | 103 | Effect speed | 0–255 |
| +4 | 104 | Effect intensity | 0–255 |
| +5 | 105 | Palette ID | 0..palette count-1 (input reg 3) |
| +6 | 106 | Custom 1 | 0–255 |
| +7 | 107 | Custom 2 | 0–255 |
| +8 | 108 | Custom 3 | 0–31 |
| +10 | 110 | Primary color Red | 0–255 |
| +11 | 111 | Primary color Green | 0–255 |
| +12 | 112 | Primary color Blue | 0–255 |
| +13 | 113 | Primary color White | 0–255 |
| +14…+17 | 114–117 | Secondary color R, G, B, W | 0–255 |
| +18…+21 | 118–121 | Tertiary color R, G, B, W | 0–255 |
| +22 | 122 | CCT (white temperature) | 0–255 |
| +23 | 123 | Reverse | 0/1 |
| +24 | 124 | Mirror | 0/1 |

Reading a segment that does not exist returns zeros (so you can poll whole
blocks); writing to one returns exception 02.

Tip: write R, G, B, W in one FC 16 request (4 registers starting at 110) so the
color changes in a single step.

Effect and palette IDs are the same numbers WLED uses in its JSON API. A full
list is at `http://<wled-ip>/json/eff` and `http://<wled-ip>/json/pal`
(the index in the array is the ID). Common effects: 0 Solid, 1 Blink, 2 Breathe,
3 Wipe, 8 Colorloop, 9 Rainbow, 12 Fade, 13 Theater.

## Input registers (FC 04, read only)

| Addr | Name |
|---|---|
| 0 | Total LED count |
| 1 | Segment count |
| 2 | Effect count |
| 3 | Palette count |
| 4 | Main segment ID |
| 5 | Current FPS |
| 6 | Estimated current draw (mA) |
| 7 | Current limit (mA, 0 = no limit) |
| 8 | Free heap (KB) |
| 9 | WiFi RSSI (signed int16, dBm) |
| 10–11 | Uptime in seconds (32-bit, high word first) |
| 12–13 | WLED build number (32-bit, high word first) |

Per segment N, block at 100 × (N + 1): +0 active (0/1), +1 start LED,
+2 stop LED (exclusive), +3 length, +4 grouping, +5 spacing.

## Coils (FC 01 read, FC 05 / 15 write)

| Addr | Name |
|---|---|
| 0 | Power |
| 1 | Nightlight active |
| 100 + N | Segment N on |
| 200 + N | Segment N reverse |
| 300 + N | Segment N mirror |

## Discrete inputs (FC 02, read only)

| Addr | Name |
|---|---|
| 0 | Power is on |
| 1 | Nightlight active |
| 2 | Live / realtime mode active (E1.31, DDP, Art-Net…) |
| 3 | Playlist running |
| 4 | Network connected |
| 100 + N | Segment N exists |

## Exceptions

| Code | When |
|---|---|
| 01 Illegal function | Unsupported function code, or any write while *Read only* is enabled |
| 02 Illegal data address | Address past 3299 (registers) / 331 (coils), or write to a missing segment |
| 03 Illegal data value | Value out of range, bad quantity, malformed request |

Multi-register / multi-coil writes check every address first, so a request
with a bad address changes nothing. Value range errors stop at the first bad
register.

## Settings

Config → Usermods → **ModbusTCP**

| Setting | Default | |
|---|---|---|
| enabled | on | |
| port | 502 | |
| unitId | 0 | 0 answers any unit/slave id; otherwise only that id is answered |
| readOnly | off | Reject all writes |
| idleTimeout | 120 | Close a client after this many seconds with no requests (0 = never) |

Live status (port, connected clients, request count) is shown on the WLED
Info page.

Modbus TCP has no authentication. Keep the controller on a trusted network,
or enable *readOnly* if you only need monitoring.

## Building

Copy this folder to `usermods/Modbus_TCP` in a WLED v16 checkout and add to
`platformio_override.ini`:

```ini
[platformio]
default_envs = esp32dev_modbus

[env:esp32dev_modbus]
extends = env:esp32dev
custom_usermods = Modbus_TCP
```

then `pio run -e esp32dev_modbus`. To keep audio reactive as well, use
`custom_usermods = audioreactive Modbus_TCP`.
