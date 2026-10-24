#!/usr/bin/env python3
"""Diagnose power-off over Modbus vs the JSON API. No extra packages needed.
   python3 tools/diag.py 192.168.68.61
Close ModMaster and any other Modbus apps first. Paste the whole output back.
"""
import json, socket, struct, sys, time, urllib.request


class Resp:
    def __init__(self, regs=None, err=None): self.registers, self.err = regs, err
    def isError(self): return self.err is not None
    def __repr__(self): return f"EXCEPTION {self.err}" if self.err is not None else f"ok {self.registers}"


class ModbusTcpClient:
    """Minimal Modbus TCP client (FC03/05/06) using only the standard library."""
    def __init__(self, host, port=502): self.host, self.port, self.tid, self.s = host, port, 0, None
    def connect(self):
        self.s = socket.create_connection((self.host, self.port), timeout=5); return True
    def close(self): self.s.close()
    def _req(self, pdu):
        self.tid = (self.tid + 1) & 0xFFFF
        self.s.sendall(struct.pack(">HHHB", self.tid, 0, len(pdu) + 1, 1) + pdu)
        hdr = self._recv(7); n = struct.unpack(">H", hdr[4:6])[0]
        return self._recv(n - 1)
    def _recv(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c: raise ConnectionError("connection closed")
            b += c
        return b
    def read_holding_registers(self, addr, count=1):
        r = self._req(struct.pack(">BHH", 3, addr, count))
        if r[0] & 0x80: return Resp(err=r[1])
        return Resp(list(struct.unpack(f">{count}H", r[2:2 + 2 * count])))
    def write_register(self, addr, val):
        r = self._req(struct.pack(">BHH", 6, addr, val)); return Resp([], r[1] if r[0] & 0x80 else None)
    def write_coil(self, addr, val):
        r = self._req(struct.pack(">BHH", 5, addr, 0xFF00 if val else 0)); return Resp([], r[1] if r[0] & 0x80 else None)


ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.68.61"

def get(path):
    with urllib.request.urlopen(f"http://{ip}{path}", timeout=5) as r:
        return json.load(r)

def post_state(d):
    req = urllib.request.Request(f"http://{ip}/json/state", data=json.dumps(d).encode(),
                                 headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=5).read()

def modbus_count():
    return get("/json/info").get("u", {}).get("Modbus TCP")

info = get("/json/info")
print("ver", info.get("ver"), "vid", info.get("vid"), "release", info.get("release"), "arch", info.get("arch"))
print("usermods", json.dumps(info.get("u")))
st = get("/json/state")
print("state", json.dumps({k: st.get(k) for k in ("on", "bri", "transition", "nl", "udpn", "lor", "ps", "pl")}))
print("seg0", json.dumps({k: st["seg"][0].get(k) for k in ("on", "bri", "fx", "col")}))
cfg = get("/json/cfg")
print("cfg.if", json.dumps(cfg.get("if"))[:600])
print("cfg.light", json.dumps(cfg.get("light")))
print("cfg.hw.relay", json.dumps(cfg.get("hw", {}).get("relay")))

c = ModbusTcpClient(ip, port=502)
assert c.connect(), "cannot connect to port 502"

def watch(label, secs=6):
    print(f"--- {label}")
    t0 = time.time()
    while time.time() - t0 < secs:
        r = c.read_holding_registers(0, count=2)
        s = get("/json/state")
        print(f"{time.time()-t0:5.1f}s  reg0/1={r.registers if not r.isError() else r}  json on={s['on']} bri={s['bri']}")
        time.sleep(0.5)

def on100():
    c.write_register(0, 1); time.sleep(1); c.write_register(1, 100); time.sleep(2)

print("modbus counter at start:", modbus_count())
print("regs 0-6:", c.read_holding_registers(0, count=7).registers)
on100(); c.write_register(0, 0);          watch("FC06 reg0 = 0 (power off)")
on100(); c.write_register(1, 0);          watch("FC06 reg1 = 0 (brightness 0)")
on100(); c.write_coil(0, False);          watch("FC05 coil0 = off")
on100(); post_state({"on": False});       watch("JSON API on:false (control, no Modbus write)")
on100()
print("modbus counter at end:", modbus_count())
c.close()
