# EtherNet/IP PCCC usermod

Lets an Allen-Bradley PLC control WLED with ordinary **MSG instructions**, the
same way it would read and write another PLC's data table. WLED answers
PCCC commands carried over EtherNet/IP (CIP *Execute PCCC*, TCP port 44818),
so no add-on profile, EDS file or tag database is needed on the PLC side.

* Works with ControlLogix / CompactLogix ("PLC5 Typed" or "SLC Typed" read and
  write), MicroLogix 1100 / 1400, SLC 5/05 and PLC-5/E (EtherNet/IP firmware),
  and HMIs or software that talk to those PLCs (RSLinx-style PCCC, pycomm3,
  libplctag, Node-RED PCCC nodes…).
* PCCC commands: PLC-5 typed read/write (`0F 68` / `0F 67`), PLC-5 word range
  read/write (`0F 01` / `0F 00`), PLC-5 read-modify-write (`0F 26`), SLC
  protected typed logical read/write with 2 or 3 address fields
  (`0F A1` `A2` / `A9` `AA`), SLC masked bit write (`0F AB`), echo (`06 00`).
* Unconnected messages, Unconnected Send (MSG routed through a 1756-ENxT or
  CompactLogix port) and connected messages (Forward Open / Large Forward
  Open, class 3) are all accepted. The *Connected* box on a Logix MSG can be on
  or off.
* Answers List Identity, so RSLinx's EtherNet/IP driver shows the controller
  (named after WLED's *Server description*).
* Up to 4 TCP clients (`-D EIP_PCCC_MAX_CLIENTS=n`) and 8 CIP connections
  (`-D EIP_PCCC_MAX_CONNECTIONS=n`).
* Polled from the WLED main loop like the Modbus TCP usermod, so it is safe
  alongside the web UI, JSON API, MQTT, sync and Modbus. Works over WiFi or
  Ethernet.
* Built against WLED **v16.0.1**.

## Data table

All files hold 16-bit words. Use **INT** tags (or INT arrays) on a Logix
controller. Values are unsigned: a value above 32767 (only the transition time
can be that big) is written as the matching negative INT.

| File | Default | Access | Contents |
|---|---|---|---|
| Control | **N7** | read/write | power, brightness, preset, transition, nightlight, main segment |
| Status | **N8** | read only | LED count, segment/effect/palette counts, FPS, current, uptime… |
| Segment *n* | **N10** + *n* (N10 = segment 0 … N41 = segment 31) | read/write | on, brightness, effect, speed, intensity, palette, colors… |
| Bits | **B3** | read/write | power, nightlight, status flags, segment on/off |

The file numbers can be changed in the usermod settings if they clash with
something you already use in your MSG logic.

### N7 control (16 elements)

| Element | Name | Range | Notes |
|---|---|---|---|
| N7:0 | Power | 0/1 | 1 = on (restores last brightness), 0 = off |
| N7:1 | Master brightness | 0–255 | 1–255 sets the level and does **not** change power (while off it sets the level used at the next power on). 0 turns off. Reads the level even while off |
| N7:2 | Preset | 0–250 | Applies a preset when the written value **changes** (writing the same number again does nothing, so a cyclic MSG is safe). Write 0, then the number, to re-apply the same preset. Reads the active preset (0 = none). A preset also sets power if it was saved with it |
| N7:3 | Transition time | 0–65535 ms | Global transition duration |
| N7:4 | Nightlight duration | 1–255 min | |
| N7:5 | Nightlight target brightness | 0–255 | |
| N7:6 | Main segment | 0..segments-1 | |
| N7:7–15 | reserved | | read 0, writes ignored |

### N8 status (16 elements, read only)

| Element | Name |
|---|---|
| N8:0 | Total LED count |
| N8:1 | Segment count |
| N8:2 | Effect count |
| N8:3 | Palette count |
| N8:4 | Main segment ID |
| N8:5 | Current FPS |
| N8:6 | Estimated current draw (mA) |
| N8:7 | Current limit (mA, 0 = no limit) |
| N8:8 | Free heap (KB) |
| N8:9 | WiFi RSSI (dBm, signed) |
| N8:10–11 | Uptime in seconds (32-bit, high word first) |
| N8:12–13 | WLED build number (32-bit, high word first) |

### N10–N41 segments (25 elements each)

Segment *n* is file N(10 + *n*). With one strip you will normally only use N10.
The layout is the same as a segment block of the Modbus TCP usermod
(element = register offset).

| Element | Name | Range |
|---|---|---|
| :0 | Segment on | 0/1 |
| :1 | Segment brightness (opacity) | 0–255 (0 turns the segment off, 1–255 only sets the level) |
| :2 | Effect ID | 0..effect count-1 (N8:2) |
| :3 | Effect speed | 0–255 |
| :4 | Effect intensity | 0–255 |
| :5 | Palette ID | 0..palette count-1 (N8:3) |
| :6 | Custom 1 | 0–255 |
| :7 | Custom 2 | 0–255 |
| :8 | Custom 3 | 0–31 |
| :10–13 | Primary color R, G, B, W | 0–255 |
| :14–17 | Secondary color R, G, B, W | 0–255 |
| :18–21 | Tertiary color R, G, B, W | 0–255 |
| :22 | CCT (white temperature) | 0–255 |
| :23 | Reverse | 0/1 |
| :24 | Mirror | 0/1 |

Reading a segment that does not exist returns zeros (so you can poll whole
files); writing to one fails with EXT STS 06.

Write N10:10 with 4 elements to change R, G, B, W in one step. Effect and
palette IDs are the numbers WLED uses in its JSON API (`http://<wled-ip>/json/eff`
and `/json/pal`, index = ID). Common effects: 0 Solid, 1 Blink, 2 Breathe,
3 Wipe, 8 Colorloop, 9 Rainbow, 12 Fade, 13 Theater.

### B3 bits (3 words)

| Address | Name | |
|---|---|---|
| B3:0/0 | Power | read/write |
| B3:0/1 | Nightlight active | read/write |
| B3:0/2 | Live / realtime mode active (E1.31, DDP, Art-Net…) | read only |
| B3:0/3 | Playlist running | read only |
| B3:1/*n* | Segment *n* on (segments 0–15) | read/write |
| B3:2/*n* | Segment 16 + *n* on (segments 16–31) | read/write |

Bit writes (SLC masked write, PLC-5 read-modify-write, a Logix MSG to
`B3:0/0`) change only that bit. Writing a whole word sets every writable bit;
bits of segments that don't exist and the read-only bits are ignored.

### Cyclic writes

Writing a value that is already set does nothing: no effect restart, no sync
packet, no UI refresh. A PLC can therefore send the same MSG every scan or on
a timer without side effects. Multi-element writes check the whole address
range first, so a write that runs past the end of a file changes nothing; a
value out of range stops the write at that element.

## Errors

The MSG instruction's error code shows the PCCC status:

| STS / EXT STS | When |
|---|---|
| STS 10h | Unsupported command, or a value out of range for that element |
| STS 60h | Any write while *Read only* is enabled in the settings |
| EXT STS 06h | No such file, wrong SLC file type (e.g. B3 read as an N file), a sub-element or bit address in an ASCII PLC-5 address, or a write to a segment that does not exist |
| EXT STS 07h | Element past the end of the file, or a zero/odd byte count |
| EXT STS 0Bh | Write to the N8 status file |
| EXT STS 0Eh | Malformed request |

## PLC setup examples

**ControlLogix / CompactLogix.** Create INT tags, e.g. `WLED_Ctrl[3]` and
`WLED_Color[4]`. MSG configuration:

* Message Type: *PLC5 Typed Write* (or *SLC Typed Write*; the read
  variants work the same way)
* Source Element: `WLED_Color[0]`, Number of Elements: `4`
* Destination Element: `N10:10`
* Communication path: the Ethernet port, then `2, <WLED IP>`, e.g.
  `LocalENB, 2, 192.168.1.50` from a CompactLogix built-in port or
  `1, <ENxT slot>, 2, 192.168.1.50` through a ControlLogix backplane
* *Connected* can be on (keeps a CIP connection open, faster for repeated
  MSGs) or off

**MicroLogix 1100 / 1400.** MSG, *Read/Write: Write*, *Target Device:
500CPU*, *Local/Remote: Local*, data table address `N7:0` locally and
`N7:0` in the target, size in elements, *Channel: 1 (Integral)*, MultiHop
*Yes*, and the WLED IP address on the MultiHop tab.

**SLC 5/05.** Same as the MicroLogix: *Target Device 500CPU*, channel 1,
MultiHop with the WLED IP address.

**PLC-5/E.** *PLC-5 Typed Write*, target `N7:0`, MultiHop with the WLED IP
address.

Older SLC 5/05 and PLC-5/E firmware that only talks CSP on TCP port 2222
(no EtherNet/IP) is not supported; neither are I/O (class 1) connections.

## Testing without a PLC

`tools/pccc_test.py <ip>` (standard library only) reads the status file,
does a PLC-5 typed read, then cycles the strip through red, green, blue and
white with SLC typed writes and switches to the Rainbow effect.
`--connected` sends the same requests over a CIP connection.

[pycomm3](https://github.com/ottowayi/pycomm3)'s `SLCDriver` and
[libplctag](https://github.com/libplctag/libplctag) (`plc=plc5`, `slc500`,
`micrologix` or `lgxpccc`) also work, for example
`tag_rw2 --type=sint16 --tag='protocol=ab_eip&gateway=<ip>&plc=slc500&name=N7:0&elem_count=3'`.

## Settings

Config → Usermods → **EIP_PCCC**

| Setting | Default | |
|---|---|---|
| enabled | on | |
| port | 44818 | EtherNet/IP explicit messaging port; PLCs always use 44818 |
| readOnly | off | Reject all writes (STS 60h) |
| idleTimeout | 120 | Close a TCP client after this many seconds with no requests (0 = never). Set it longer than the slowest MSG period, or 0 |
| discovery | on | Answer List Identity broadcasts on UDP 44818 (RSLinx browsing) |
| controlFile | 7 | N file number of the control file |
| statusFile | 8 | N file number of the status file |
| segmentFile | 10 | N file number of segment 0; segment *n* is this + *n* |
| bitFile | 3 | B file number |

Live status (port, TCP clients, CIP connections, request count) is shown on
the WLED Info page.

EtherNet/IP and PCCC have no authentication. Keep the controller on a trusted
(control) network, or enable *readOnly* if the PLC only needs to monitor.

## Ethernet

Build with an Ethernet environment (`esp32_eth_pccc` in
`platformio_override.ini`) and select your board under Config → WiFi Setup →
**Ethernet Type** (WT32-ETH01 for the EternaGlo Plus). The server listens on
whichever interface is up; use the Ethernet IP address in the MSG path. A
static IP set on the WiFi Setup page applies to Ethernet too, which is what
you want for a PLC.

## Building

Copy this folder to `usermods/EIP_PCCC` in a WLED v16 checkout and add to
`platformio_override.ini`:

```ini
[env:esp32_eth_pccc]
extends = env:esp32_eth
custom_usermods = EIP_PCCC
```

then `pio run -e esp32_eth_pccc`. It can be combined with the Modbus TCP
usermod: `custom_usermods = Modbus_TCP EIP_PCCC`.
