# Installing the Modbus TCP firmware on the EternaGlo Plus

There are two builds. Both are WLED 16.0.1 for classic ESP32 (4 MB flash) plus
the Modbus TCP usermod, without the audio reactive usermod.

* **`WLED_16.0.1_ESP32_Ethernet_ModbusTCP.bin` (use this one on the EternaGlo
  Plus)**: includes Ethernet support. Release name `ESP32_Ethernet`.
* `WLED_16.0.1_ESP32_ModbusTCP.bin`: WiFi only, release name `ESP32`.
  Flashing this one removes Ethernet support.

## Ethernet

After flashing the Ethernet build, go to Config → **WiFi Setup** → **Ethernet
Type** and select **WT32-ETH01** (the profile EternaGlo lists for the Plus),
save, and reboot. Plug in the cable; the controller gets a DHCP address on
Ethernet (check your router or the Info page). Modbus TCP listens on port 502
on whichever interface is up, Ethernet or WiFi.

WLED uses Ethernet in place of WiFi when the cable is connected, so use the
Ethernet IP address in your Modbus client. Setting a static IP on the WiFi
Setup page applies to Ethernet as well, which is handy for a PLC.

## 1. Check the controller first

Open the WLED UI → **Info**. Confirm:

* the chip is a plain **ESP32** (not ESP32-S2/S3/C3). If it is not, don't flash
  this file; ask for a rebuild for that chip.
* note the **release name / version** shown. If it is not `ESP32`, the update
  page will refuse the file with "release name mismatch" (see step 3).

Back up first: Config → **Security & Updates** → *Backup presets* and
*Backup configuration*.

## 2. Flash over WiFi (OTA)

Config → **Security & Updates** → **Manual OTA update** → choose the `.bin` →
**Upload**. The controller reboots in about 30 seconds with settings kept.

## 3. If the upload is refused

* "Firmware release name mismatch": the EternaGlo shipped with a vendor build.
  Tick **Ignore firmware validation** and upload again, only if step 1 showed a
  classic ESP32.
* If OTA fails for another reason (for example coming from a much older WLED),
  flash over USB with esptool (`esptool.py write_flash 0x10000 <bin>`) or
  install stock WLED 16 from https://install.wled.me first, then OTA this file,
  then restore the backups.

## 4. LED settings for the WS2814 RGBW strip

Config → **LED Preferences**:

* LED type: **SK6812/WS2814 RGBW**, length **60**, GPIO as already configured.
* Color order: start with **RGB**. Run `tools/modbus_test.py`: it shows red, green,
  blue, white for 2 s each. If they come out wrong, change the color order
  (WS2814 strips vary; some need GRB or the "swap W" option).
* **White management → Auto-calculate white channel: None** if you want to set
  the W register directly over Modbus. Other modes recompute W from RGB and
  will override what you write.

## 5. Turn on Modbus

Config → **Usermods → ModbusTCP**: enabled (default on), port 502. The Info
page shows the port and connected clients.

Test from a PC on the same network:

```
pip install pymodbus
python3 tools/modbus_test.py <controller-ip>
```

The register map is in `usermods/Modbus_TCP/readme.md`.
