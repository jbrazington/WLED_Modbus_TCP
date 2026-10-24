#!/usr/bin/env python3
"""Quick check of the WLED EtherNet/IP PCCC usermod, standard library only:
   python3 pccc_test.py 192.168.1.50 [--connected]

Talks to WLED the way a PLC MSG instruction does: SLC typed read/write
(MicroLogix/SLC style) and PLC-5 typed read (Logix "PLC5 Typed Read"), as PCCC
inside CIP Execute PCCC. --connected opens a CIP connection first (Forward
Open), like a Logix MSG with "Connected" ticked.
"""
import socket, struct, sys, time

ip = next((a for a in sys.argv[1:] if not a.startswith("-")), "192.168.1.50")
connected = "--connected" in sys.argv
s = socket.create_connection((ip, 44818), timeout=3)

def encap(cmd, data, session=0):
    s.sendall(struct.pack("<HHII8sI", cmd, len(data), session, 0, b"pccctest", 0) + data)
    hdr = recv(24)
    cmd_r, length, session_r, status = struct.unpack("<HHII", hdr[:12])
    body = recv(length)
    if status: sys.exit(f"encapsulation command 0x{cmd:04x} failed, status 0x{status:x}")
    return session_r, body

def recv(n):
    b = b""
    while len(b) < n:
        chunk = s.recv(n - len(b))
        if not chunk: sys.exit("connection closed")
        b += chunk
    return b

session, _ = encap(0x65, struct.pack("<HH", 1, 0))
conn_id, seq = None, 0

def cip(req):
    global seq
    if conn_id is None:
        _, body = encap(0x6F, struct.pack("<IHH HH HH", 0, 0, 2, 0, 0, 0xB2, len(req)) + req, session)
        return body[16:]
    seq += 1
    _, body = encap(0x70, struct.pack("<IHH HHI HHH", 0, 0, 2, 0xA1, 4, conn_id, 0xB1, len(req) + 2, seq) + req, session)
    return body[22:]

if connected:
    fo = struct.pack("<BB II HHI B3x IH IH B", 0x0A, 0x0E, 0, 0x12345678, 1, 0x4D, 0xC0FFEE, 1,
                     2000000, 0x43F4, 2000000, 0x43F4, 0xA3) + bytes([2, 0x20, 0x02, 0x24, 0x01])
    r = cip(bytes([0x54, 2, 0x20, 0x06, 0x24, 0x01]) + fo)
    if r[2]: sys.exit(f"Forward Open failed, CIP status 0x{r[2]:02x}")
    conn_id = struct.unpack("<I", r[4:8])[0]
    print(f"connected, connection id 0x{conn_id:08x}")

tns = 0
def pccc(cmd):
    global tns
    tns += 1
    req = bytes([0x4B, 2, 0x20, 0x67, 0x24, 0x01, 7]) + struct.pack("<HI", 0x4D, 0xC0FFEE) + bytes([0x0F, 0]) + struct.pack("<H", tns) + cmd
    r = cip(req)
    if r[2]: sys.exit(f"CIP status 0x{r[2]:02x}")
    p = r[4 + r[4]:]
    if p[1]: sys.exit(f"PCCC STS 0x{p[1]:02x}" + (f" EXT 0x{p[4]:02x}" if p[1] == 0xF0 else ""))
    return p[4:]

def read(file, elem, n):  # SLC typed read, N file
    return list(struct.unpack(f"<{n}h", pccc(bytes([0xA2, n * 2, file, 0x89, elem, 0]))))

def write(file, elem, values):  # SLC typed write, N file
    pccc(bytes([0xAA, len(values) * 2, file, 0x89, elem, 0]) + struct.pack(f"<{len(values)}h", *values))

st = read(8, 0, 14)
print(f"N8 status: LEDs={st[0]} segments={st[1]} effects={st[2]} palettes={st[3]} fps={st[5]} mA={st[6]}")
print("N7:0-2 power/bri/preset:", read(7, 0, 3))

d = pccc(bytes([0x68, 0, 0, 3, 0, 0x06, 7, 0, 3, 0]))  # PLC-5 typed read N7:0, 3 elements
print("PLC-5 typed read N7:0:", list(struct.unpack("<3h", d[3:])))

write(7, 0, [1, 128])                 # power on, 50% brightness
write(10, 2, [0])                     # segment 0 effect: Solid
for name, rgbw in [("red", [255, 0, 0, 0]), ("green", [0, 255, 0, 0]), ("blue", [0, 0, 255, 0]), ("white", [0, 0, 0, 255])]:
    print("color:", name)
    write(10, 10, rgbw)               # N10:10-13 segment 0 primary R,G,B,W
    time.sleep(2)
write(10, 2, [9, 128, 128])           # effect Rainbow, speed, intensity
print("effect Rainbow, N10:0-24:", read(10, 0, 25))

if conn_id is not None:
    fc = struct.pack("<BB HHI", 0x0A, 0x0E, 1, 0x4D, 0xC0FFEE) + bytes([3, 0, 0x20, 0x02, 0x24, 0x01])
    conn_id = None
    cip(bytes([0x4E, 2, 0x20, 0x06, 0x24, 0x01]) + fc)
s.sendall(struct.pack("<HHII8sI", 0x66, 0, session, 0, b"pccctest", 0))  # UnRegister Session
s.close()
