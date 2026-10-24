#!/usr/bin/env python3
"""Quick check of the WLED Modbus TCP usermod.  pip install pymodbus  then:
   python3 modbus_test.py 192.168.1.50
"""
import sys, time
from pymodbus.client import ModbusTcpClient

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.50"
c = ModbusTcpClient(ip, port=502)
if not c.connect():
    sys.exit(f"could not connect to {ip}:502")

def rd(addr, n=1, inp=False):
    r = (c.read_input_registers if inp else c.read_holding_registers)(addr, count=n)
    if r.isError(): sys.exit(f"read {addr} failed: {r}")
    return r.registers

info = rd(0, 14, inp=True)
print(f"LEDs={info[0]} segments={info[1]} effects={info[2]} palettes={info[3]} fps={info[5]} mA={info[6]} rssi={info[9]-65536 if info[9] > 32767 else info[9]}")
print("power/bri:", rd(0, 2))

c.write_register(0, 1)                         # on
c.write_register(1, 128)                       # 50% brightness
c.write_register(102, 0)                       # segment 0 effect: Solid
for name, rgbw in [("red", [255,0,0,0]), ("green", [0,255,0,0]), ("blue", [0,0,255,0]), ("white", [0,0,0,255])]:
    print("color:", name)
    c.write_registers(110, rgbw)               # segment 0 primary R,G,B,W
    time.sleep(2)
c.write_registers(102, [9, 128, 128])          # effect Rainbow, speed, intensity
print("effect Rainbow, segment 0 block:", rd(100, 25))
c.close()
