#include "wled.h"

/*
 * Modbus TCP server usermod
 *
 * Exposes WLED state (power, brightness, presets, nightlight and per-segment
 * effect, palette, speed, intensity and RGBW colors) as Modbus coils and
 * registers so a PLC, SCADA or home automation system can control WLED.
 *
 * The server is polled from loop() (no async callbacks), so all state changes
 * happen on the main WLED task. See readme.md for the full register map.
 */

#ifndef MODBUS_TCP_PORT
  #define MODBUS_TCP_PORT 502
#endif
#ifndef MODBUS_TCP_MAX_CLIENTS
  #define MODBUS_TCP_MAX_CLIENTS 4
#endif

#define MB_MAX_SEGMENTS   32    // segments addressable through the register map
#define MB_SEG_BASE       100   // segment N block starts at MB_SEG_BASE * (N+1)
#define MB_MAX_ADDR       (MB_SEG_BASE * (MB_MAX_SEGMENTS + 1))
#define MB_FRAME_SIZE     260   // 7 byte MBAP header + 253 byte PDU

// function codes
#define MB_FC_READ_COILS          0x01
#define MB_FC_READ_DISCRETE       0x02
#define MB_FC_READ_HOLDING        0x03
#define MB_FC_READ_INPUT          0x04
#define MB_FC_WRITE_COIL          0x05
#define MB_FC_WRITE_REGISTER      0x06
#define MB_FC_WRITE_COILS         0x0F
#define MB_FC_WRITE_REGISTERS     0x10

// exception codes
#define MB_EX_ILLEGAL_FUNCTION    0x01
#define MB_EX_ILLEGAL_ADDRESS     0x02
#define MB_EX_ILLEGAL_VALUE       0x03

// global holding registers
#define MB_HR_POWER               0
#define MB_HR_BRIGHTNESS          1
#define MB_HR_PRESET              2
#define MB_HR_TRANSITION          3
#define MB_HR_NL_DURATION         4
#define MB_HR_NL_TARGET_BRI       5
#define MB_HR_MAIN_SEGMENT        6

// per-segment holding register offsets
#define MB_SEG_ON                 0
#define MB_SEG_OPACITY            1
#define MB_SEG_EFFECT             2
#define MB_SEG_SPEED              3
#define MB_SEG_INTENSITY          4
#define MB_SEG_PALETTE            5
#define MB_SEG_CUSTOM1            6
#define MB_SEG_CUSTOM2            7
#define MB_SEG_CUSTOM3            8
#define MB_SEG_COLOR              10  // 10..21: 3 colors x R,G,B,W
#define MB_SEG_CCT                22
#define MB_SEG_REVERSE            23
#define MB_SEG_MIRROR             24

// global coils / discrete inputs
#define MB_CO_POWER               0
#define MB_CO_NIGHTLIGHT          1
#define MB_CO_SEG_ON              100 // 100+N
#define MB_CO_SEG_REVERSE         200 // 200+N
#define MB_CO_SEG_MIRROR          300 // 300+N
#define MB_CO_MAX                 (MB_CO_SEG_MIRROR + MB_MAX_SEGMENTS)

class ModbusTCPUsermod : public Usermod {

  private:

    struct Client {
      WiFiClient sock;
      uint8_t buf[MB_FRAME_SIZE];
      uint16_t len = 0;
      unsigned long lastSeen = 0;
    };

    bool enabled = true;
    bool readOnly = false;
    uint16_t port = MODBUS_TCP_PORT;
    uint8_t unitId = 0;               // 0 = answer any unit id
    uint16_t idleTimeout = 120;       // seconds, 0 = never
    bool initDone = false;

    WiFiServer *server = nullptr;
    uint16_t serverPort = 0;
    Client clients[MODBUS_TCP_MAX_CLIENTS];
    uint32_t requestCount = 0;

    bool changed = false;             // a write changed WLED state, call stateUpdated()
    uint16_t lastPresetWritten = 0;   // preset register is edge triggered, see writeHolding()

    static const char _name[];
    static const char _enabled[];
    static const char _readOnly[];
    static const char _port[];
    static const char _unitId[];
    static const char _idleTimeout[];

    void startServer();
    void stopServer();
    void serviceClient(Client &c);
    uint16_t processFrame(uint8_t *frame, uint16_t len);  // returns response length

    // register access, return false on illegal address
    bool readHolding(uint16_t addr, uint16_t &val);
    bool readInput(uint16_t addr, uint16_t &val);
    bool readCoil(uint16_t addr, bool &val);
    bool readDiscrete(uint16_t addr, bool &val);
    uint8_t writeHolding(uint16_t addr, uint16_t val);   // returns exception code or 0
    uint8_t writeCoil(uint16_t addr, bool val);

    static Segment *segmentAt(uint16_t idx) {
      if (idx >= strip.getSegmentsNum()) return nullptr;
      return &strip.getSegment(idx);
    }

  public:

    void setup() override {
      initDone = true;
    }

    void connected() override {
      if (enabled) startServer();
    }

    void loop() override {
      if (!enabled || !server) return;

      // accept new connections
      WiFiClient incoming = server->available();
      if (incoming) {
        int slot = -1;
        for (int i = 0; i < MODBUS_TCP_MAX_CLIENTS; i++) {
          if (!clients[i].sock || !clients[i].sock.connected()) { slot = i; break; }
        }
        if (slot >= 0) {
          clients[slot].sock.stop();
          clients[slot].sock = incoming;
          clients[slot].sock.setNoDelay(true);
          clients[slot].len = 0;
          clients[slot].lastSeen = millis();
          DEBUG_PRINTF_P(PSTR("Modbus: client %d connected\n"), slot);
        } else {
          incoming.stop(); // no free slot
        }
      }

      for (int i = 0; i < MODBUS_TCP_MAX_CLIENTS; i++) serviceClient(clients[i]);

      if (changed) {
        changed = false;
        stateUpdated(CALL_MODE_DIRECT_CHANGE);
      }
    }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray info = user.createNestedArray(F("Modbus TCP"));
      if (!enabled) { info.add(F("disabled")); return; }
      if (!server)  { info.add(F("waiting for network")); return; }
      unsigned n = 0;
      for (int i = 0; i < MODBUS_TCP_MAX_CLIENTS; i++) if (clients[i].sock && clients[i].sock.connected()) n++;
      char buf[48];
      snprintf_P(buf, sizeof(buf), PSTR("port %u, %u client(s), %u req"), serverPort, n, (unsigned)requestCount);
      info.add(buf);
    }

    void addToConfig(JsonObject &root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)]     = enabled;
      top[FPSTR(_port)]        = port;
      top[FPSTR(_unitId)]      = unitId;
      top[FPSTR(_readOnly)]    = readOnly;
      top[FPSTR(_idleTimeout)] = idleTimeout;
    }

    bool readFromConfig(JsonObject &root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled, true);
      configComplete &= getJsonValue(top[FPSTR(_port)], port, (uint16_t)MODBUS_TCP_PORT);
      configComplete &= getJsonValue(top[FPSTR(_unitId)], unitId, (uint8_t)0);
      configComplete &= getJsonValue(top[FPSTR(_readOnly)], readOnly, false);
      configComplete &= getJsonValue(top[FPSTR(_idleTimeout)], idleTimeout, (uint16_t)120);
      if (port == 0) port = MODBUS_TCP_PORT;

      // apply changes made on the settings page
      if (initDone) {
        if (!enabled) stopServer();
        else if (WLED_CONNECTED && (!server || serverPort != port)) startServer();
      }
      return configComplete;
    }

    void appendConfigData() override {
      oappend(F("addInfo('ModbusTCP:unitId',1,'(0 = respond to any unit id)');"));
      oappend(F("addInfo('ModbusTCP:idleTimeout',1,'seconds (0 = never)');"));
      oappend(F("addInfo('ModbusTCP:readOnly',1,'(reject all writes)');"));
    }
};

const char ModbusTCPUsermod::_name[]        PROGMEM = "ModbusTCP";
const char ModbusTCPUsermod::_enabled[]     PROGMEM = "enabled";
const char ModbusTCPUsermod::_readOnly[]    PROGMEM = "readOnly";
const char ModbusTCPUsermod::_port[]        PROGMEM = "port";
const char ModbusTCPUsermod::_unitId[]      PROGMEM = "unitId";
const char ModbusTCPUsermod::_idleTimeout[] PROGMEM = "idleTimeout";


void ModbusTCPUsermod::startServer() {
  if (server && serverPort == port) return;
  stopServer();
  server = new WiFiServer(port);
  server->begin();
  server->setNoDelay(true);
  serverPort = port;
  DEBUG_PRINTF_P(PSTR("Modbus: listening on port %u\n"), port);
}

void ModbusTCPUsermod::stopServer() {
  for (int i = 0; i < MODBUS_TCP_MAX_CLIENTS; i++) { clients[i].sock.stop(); clients[i].len = 0; }
  if (server) {
    server->end();
    delete server;
    server = nullptr;
  }
  serverPort = 0;
}

void ModbusTCPUsermod::serviceClient(Client &c) {
  if (!c.sock) return;
  if (!c.sock.connected()) { c.sock.stop(); c.len = 0; return; }

  unsigned long now = millis();
  int avail = c.sock.available();
  if (avail <= 0) {
    if (idleTimeout && now - c.lastSeen > idleTimeout * 1000UL) { c.sock.stop(); c.len = 0; }
    return;
  }
  c.lastSeen = now;

  // read what fits, then handle every complete frame in the buffer
  int n = c.sock.read(c.buf + c.len, min((int)(MB_FRAME_SIZE - c.len), avail));
  if (n > 0) c.len += n;

  while (c.len >= 7) {
    uint16_t proto = (c.buf[2] << 8) | c.buf[3];
    uint16_t mbLen = (c.buf[4] << 8) | c.buf[5]; // unit id + PDU
    if (proto != 0 || mbLen < 2 || mbLen > MB_FRAME_SIZE - 6) { c.sock.stop(); c.len = 0; return; } // garbage, drop client
    uint16_t frameLen = 6 + mbLen;
    if (c.len < frameLen) break; // wait for the rest

    uint8_t resp[MB_FRAME_SIZE];
    memcpy(resp, c.buf, frameLen);
    uint16_t respLen = processFrame(resp, frameLen);
    if (respLen) c.sock.write(resp, respLen);
    requestCount++;

    memmove(c.buf, c.buf + frameLen, c.len - frameLen);
    c.len -= frameLen;
  }
}

/*
 * Handles one MBAP frame in place. frame[] holds the request on entry and the
 * response on return. Returns 0 if no response should be sent.
 */
uint16_t ModbusTCPUsermod::processFrame(uint8_t *frame, uint16_t len) {
  uint8_t unit = frame[6];
  if (unitId && unit != unitId) return 0; // not for us (as a gateway would, stay silent)

  uint8_t  *pdu    = frame + 7;
  uint16_t pduLen  = len - 7;
  uint8_t  fc      = pdu[0];
  uint8_t  ex      = 0;
  uint16_t outLen  = 0; // response PDU length

  uint16_t addr = pduLen >= 3 ? (pdu[1] << 8) | pdu[2] : 0;
  uint16_t qty  = pduLen >= 5 ? (pdu[3] << 8) | pdu[4] : 0;

  switch (fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE: {
      if (pduLen != 5 || qty < 1 || qty > 2000) { ex = MB_EX_ILLEGAL_VALUE; break; }
      uint8_t bytes = (qty + 7) / 8;
      uint8_t out[250] = {0};
      for (uint16_t i = 0; i < qty && !ex; i++) {
        bool v = false;
        bool ok = (fc == MB_FC_READ_COILS) ? readCoil(addr + i, v) : readDiscrete(addr + i, v);
        if (!ok) ex = MB_EX_ILLEGAL_ADDRESS;
        else if (v) out[i >> 3] |= 1 << (i & 7);
      }
      if (ex) break;
      pdu[1] = bytes;
      memcpy(pdu + 2, out, bytes);
      outLen = 2 + bytes;
      break;
    }

    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT: {
      if (pduLen != 5 || qty < 1 || qty > 125) { ex = MB_EX_ILLEGAL_VALUE; break; }
      uint16_t vals[125];
      for (uint16_t i = 0; i < qty && !ex; i++) {
        bool ok = (fc == MB_FC_READ_HOLDING) ? readHolding(addr + i, vals[i]) : readInput(addr + i, vals[i]);
        if (!ok) ex = MB_EX_ILLEGAL_ADDRESS;
      }
      if (ex) break;
      pdu[1] = qty * 2;
      for (uint16_t i = 0; i < qty; i++) { pdu[2 + i*2] = vals[i] >> 8; pdu[3 + i*2] = vals[i] & 0xFF; }
      outLen = 2 + qty * 2;
      break;
    }

    case MB_FC_WRITE_COIL: {
      if (pduLen != 5 || (qty != 0xFF00 && qty != 0x0000)) { ex = MB_EX_ILLEGAL_VALUE; break; }
      if (readOnly) { ex = MB_EX_ILLEGAL_FUNCTION; break; }
      ex = writeCoil(addr, qty == 0xFF00);
      outLen = 5; // echo request
      break;
    }

    case MB_FC_WRITE_REGISTER: {
      if (pduLen != 5) { ex = MB_EX_ILLEGAL_VALUE; break; }
      if (readOnly) { ex = MB_EX_ILLEGAL_FUNCTION; break; }
      ex = writeHolding(addr, qty); // qty field holds the value for FC06
      outLen = 5; // echo request
      break;
    }

    case MB_FC_WRITE_COILS: {
      uint8_t bytes = pduLen >= 6 ? pdu[5] : 0;
      if (qty < 1 || qty > 1968 || bytes != (qty + 7) / 8 || pduLen != 6 + bytes) { ex = MB_EX_ILLEGAL_VALUE; break; }
      if (readOnly) { ex = MB_EX_ILLEGAL_FUNCTION; break; }
      // validate the whole range first so a bad address does not leave a half-applied write
      for (uint16_t i = 0; i < qty; i++) { bool v; if (!readCoil(addr + i, v)) { ex = MB_EX_ILLEGAL_ADDRESS; break; } }
      for (uint16_t i = 0; i < qty && !ex; i++) ex = writeCoil(addr + i, pdu[6 + (i >> 3)] & (1 << (i & 7)));
      outLen = 5; // address + quantity
      break;
    }

    case MB_FC_WRITE_REGISTERS: {
      uint8_t bytes = pduLen >= 6 ? pdu[5] : 0;
      if (qty < 1 || qty > 123 || bytes != qty * 2 || pduLen != 6 + bytes) { ex = MB_EX_ILLEGAL_VALUE; break; }
      if (readOnly) { ex = MB_EX_ILLEGAL_FUNCTION; break; }
      for (uint16_t i = 0; i < qty; i++) { uint16_t v; if (!readHolding(addr + i, v)) { ex = MB_EX_ILLEGAL_ADDRESS; break; } }
      for (uint16_t i = 0; i < qty && !ex; i++) ex = writeHolding(addr + i, (pdu[6 + i*2] << 8) | pdu[7 + i*2]);
      outLen = 5; // address + quantity
      break;
    }

    default:
      ex = MB_EX_ILLEGAL_FUNCTION;
  }

  if (ex) {
    pdu[0] = fc | 0x80;
    pdu[1] = ex;
    outLen = 2;
  }

  uint16_t mbLen = outLen + 1; // unit id + PDU
  frame[4] = mbLen >> 8;
  frame[5] = mbLen & 0xFF;
  return 6 + mbLen;
}


bool ModbusTCPUsermod::readHolding(uint16_t addr, uint16_t &val) {
  val = 0;
  if (addr >= MB_MAX_ADDR) return false;

  if (addr < MB_SEG_BASE) {
    switch (addr) {
      case MB_HR_POWER:         val = bri > 0; break;
      case MB_HR_BRIGHTNESS:    val = bri ? bri : briLast; break; // level used when on
      case MB_HR_PRESET:        val = currentPreset; break;
      case MB_HR_TRANSITION:    val = transitionDelay; break;
      case MB_HR_NL_DURATION:   val = nightlightDelayMins; break;
      case MB_HR_NL_TARGET_BRI: val = nightlightTargetBri; break;
      case MB_HR_MAIN_SEGMENT:  val = strip.getMainSegmentId(); break;
    }
    return true; // unused addresses read as 0
  }

  Segment *seg = segmentAt(addr / MB_SEG_BASE - 1);
  if (!seg) return true; // missing segment reads as 0 so whole blocks can be polled
  unsigned off = addr % MB_SEG_BASE;
  if (off >= MB_SEG_COLOR && off < MB_SEG_COLOR + 3*4) {
    unsigned slot = (off - MB_SEG_COLOR) / 4, ch = (off - MB_SEG_COLOR) % 4;
    uint32_t c = seg->colors[slot];
    switch (ch) { case 0: val = R(c); break; case 1: val = G(c); break; case 2: val = B(c); break; default: val = W(c); }
    return true;
  }
  switch (off) {
    case MB_SEG_ON:        val = seg->on; break;
    case MB_SEG_OPACITY:   val = seg->opacity; break;
    case MB_SEG_EFFECT:    val = seg->mode; break;
    case MB_SEG_SPEED:     val = seg->speed; break;
    case MB_SEG_INTENSITY: val = seg->intensity; break;
    case MB_SEG_PALETTE:   val = seg->palette; break;
    case MB_SEG_CUSTOM1:   val = seg->custom1; break;
    case MB_SEG_CUSTOM2:   val = seg->custom2; break;
    case MB_SEG_CUSTOM3:   val = seg->custom3; break;
    case MB_SEG_CCT:       val = seg->cct; break;
    case MB_SEG_REVERSE:   val = seg->reverse; break;
    case MB_SEG_MIRROR:    val = seg->mirror; break;
  }
  return true;
}

uint8_t ModbusTCPUsermod::writeHolding(uint16_t addr, uint16_t val) {
  if (addr >= MB_MAX_ADDR) return MB_EX_ILLEGAL_ADDRESS;

  // PLCs and HMIs often write their whole register block every cycle: writing the
  // current value must be a no-op, or every poll restarts effects and spams sync/UI
  uint16_t cur;
  if (addr != MB_HR_PRESET && readHolding(addr, cur) && cur == val && (addr < MB_SEG_BASE || segmentAt(addr / MB_SEG_BASE - 1))) return 0;

  if (addr < MB_SEG_BASE) {
    switch (addr) {
      case MB_HR_POWER:
        if (val > 1) return MB_EX_ILLEGAL_VALUE;
        return writeCoil(MB_CO_POWER, val);
      case MB_HR_BRIGHTNESS:
        if (val > 255) return MB_EX_ILLEGAL_VALUE;
        // 0 turns off; otherwise set the level without touching power, so a
        // block write of power=0 + brightness=200 stays off
        if (val == 0) { if (bri) toggleOnOff(); }
        else if (bri) bri = val;
        else briLast = val; // used by the next power on
        changed = true;
        break;
      case MB_HR_PRESET:
        // Apply only when the written value changes. Comparing with currentPreset is not
        // enough: any other change (e.g. power off) clears currentPreset, so a client that
        // keeps writing the same preset would re-apply it, and with it the preset's power state.
        // Write 0 to re-arm, so the same preset can be applied again.
        if (val > 250) return MB_EX_ILLEGAL_VALUE;
        if (val == lastPresetWritten) break;
        lastPresetWritten = val;
        if (val == 0) break;
        applyPreset(val, CALL_MODE_DIRECT_CHANGE); // loads asynchronously and notifies by itself
        break;
      case MB_HR_TRANSITION:
        transitionDelay = val;
        strip.setTransition(val);
        break;
      case MB_HR_NL_DURATION:
        if (val < 1 || val > 255) return MB_EX_ILLEGAL_VALUE;
        nightlightDelayMins = val;
        break;
      case MB_HR_NL_TARGET_BRI:
        if (val > 255) return MB_EX_ILLEGAL_VALUE;
        nightlightTargetBri = val;
        break;
      case MB_HR_MAIN_SEGMENT:
        if (val >= strip.getSegmentsNum()) return MB_EX_ILLEGAL_VALUE;
        strip.setMainSegmentId(val);
        changed = true;
        break;
      default: break; // unused addresses accept and ignore writes
    }
    return 0;
  }

  Segment *seg = segmentAt(addr / MB_SEG_BASE - 1);
  if (!seg) return MB_EX_ILLEGAL_ADDRESS; // can't write a segment that doesn't exist
  unsigned off = addr % MB_SEG_BASE;

  if (off >= MB_SEG_COLOR && off < MB_SEG_COLOR + 3*4) {
    if (val > 255) return MB_EX_ILLEGAL_VALUE;
    unsigned slot = (off - MB_SEG_COLOR) / 4, ch = (off - MB_SEG_COLOR) % 4;
    uint32_t c = seg->colors[slot];
    uint8_t r = R(c), g = G(c), b = B(c), w = W(c);
    switch (ch) { case 0: r = val; break; case 1: g = val; break; case 2: b = val; break; default: w = val; }
    seg->setColor(slot, RGBW32(r, g, b, w));
    changed = true;
    return 0;
  }

  switch (off) {
    case MB_SEG_ON:
      if (val > 1) return MB_EX_ILLEGAL_VALUE;
      seg->setOption(SEG_OPTION_ON, val);
      break;
    case MB_SEG_OPACITY:
      if (val > 255) return MB_EX_ILLEGAL_VALUE;
      // same rule as master brightness: 0 turns the segment off, otherwise only the level changes
      if (val > 0) seg->setOpacity(val);
      else seg->setOption(SEG_OPTION_ON, false);
      break;
    case MB_SEG_EFFECT:
      if (val >= strip.getModeCount()) return MB_EX_ILLEGAL_VALUE;
      if (currentPlaylist >= 0) unloadPlaylist();
      if (val != seg->mode) seg->setMode(val);
      break;
    case MB_SEG_SPEED:
      if (val > 255) return MB_EX_ILLEGAL_VALUE;
      seg->speed = val;
      stateChanged = true;
      break;
    case MB_SEG_INTENSITY:
      if (val > 255) return MB_EX_ILLEGAL_VALUE;
      seg->intensity = val;
      stateChanged = true;
      break;
    case MB_SEG_PALETTE:
      if (val >= getPaletteCount()) return MB_EX_ILLEGAL_VALUE;
      seg->setPalette(val);
      break;
    case MB_SEG_CUSTOM1:
      if (val > 255) return MB_EX_ILLEGAL_VALUE;
      seg->custom1 = val;
      stateChanged = true;
      break;
    case MB_SEG_CUSTOM2:
      if (val > 255) return MB_EX_ILLEGAL_VALUE;
      seg->custom2 = val;
      stateChanged = true;
      break;
    case MB_SEG_CUSTOM3:
      if (val > 31) return MB_EX_ILLEGAL_VALUE;
      seg->custom3 = val;
      stateChanged = true;
      break;
    case MB_SEG_CCT:
      if (val > 255) return MB_EX_ILLEGAL_VALUE;
      seg->setCCT(val);
      break;
    case MB_SEG_REVERSE:
      if (val > 1) return MB_EX_ILLEGAL_VALUE;
      seg->reverse = val;
      stateChanged = true;
      break;
    case MB_SEG_MIRROR:
      if (val > 1) return MB_EX_ILLEGAL_VALUE;
      seg->mirror = val;
      stateChanged = true;
      break;
    default: return 0; // reserved offsets accept and ignore writes
  }
  changed = true;
  return 0;
}

bool ModbusTCPUsermod::readInput(uint16_t addr, uint16_t &val) {
  val = 0;
  if (addr >= MB_MAX_ADDR) return false;

  if (addr < MB_SEG_BASE) {
    uint32_t up = millis() / 1000;
    uint32_t ver = VERSION;
    switch (addr) {
      case 0:  val = strip.getLengthTotal(); break;
      case 1:  val = strip.getSegmentsNum(); break;
      case 2:  val = strip.getModeCount(); break;
      case 3:  val = getPaletteCount(); break;
      case 4:  val = strip.getMainSegmentId(); break;
      case 5:  val = strip.getFps(); break;
      case 6:  val = BusManager::currentMilliamps(); break;
      case 7:  val = BusManager::ablMilliampsMax(); break;
      case 8:  val = ESP.getFreeHeap() / 1024; break;
      case 9:  val = (uint16_t)(int16_t)WiFi.RSSI(); break;
      case 10: val = up >> 16; break;
      case 11: val = up & 0xFFFF; break;
      case 12: val = ver >> 16; break;
      case 13: val = ver & 0xFFFF; break;
    }
    return true;
  }

  Segment *seg = segmentAt(addr / MB_SEG_BASE - 1);
  if (!seg) return true;
  switch (addr % MB_SEG_BASE) {
    case 0: val = seg->isActive(); break;
    case 1: val = seg->start; break;
    case 2: val = seg->stop; break;
    case 3: val = seg->length(); break;
    case 4: val = seg->grouping; break;
    case 5: val = seg->spacing; break;
  }
  return true;
}

bool ModbusTCPUsermod::readCoil(uint16_t addr, bool &val) {
  val = false;
  if (addr >= MB_CO_MAX) return false;
  if (addr < MB_CO_SEG_ON) {
    if (addr == MB_CO_POWER) val = bri > 0;
    else if (addr == MB_CO_NIGHTLIGHT) val = nightlightActive;
    return true;
  }
  Segment *seg = segmentAt(addr % 100);
  if (!seg || addr % 100 >= MB_MAX_SEGMENTS) return true;
  if (addr < MB_CO_SEG_REVERSE) val = seg->on;
  else if (addr < MB_CO_SEG_MIRROR) val = seg->reverse;
  else val = seg->mirror;
  return true;
}

uint8_t ModbusTCPUsermod::writeCoil(uint16_t addr, bool val) {
  if (addr >= MB_CO_MAX) return MB_EX_ILLEGAL_ADDRESS;
  bool cur;
  if (readCoil(addr, cur) && cur == val && (addr < MB_CO_SEG_ON || segmentAt(addr % 100))) return 0; // unchanged, see writeHolding()
  if (addr < MB_CO_SEG_ON) {
    if (addr == MB_CO_POWER) {
      if (val != (bri > 0)) toggleOnOff();
      if (val) for (size_t s = 0; s < strip.getSegmentsNum(); s++) strip.getSegment(s).freeze = false; // like the JSON API
      changed = true;
    } else if (addr == MB_CO_NIGHTLIGHT) {
      nightlightActive = val;
      changed = true;
    }
    return 0;
  }
  unsigned idx = addr % 100;
  if (idx >= MB_MAX_SEGMENTS) return 0; // gap between blocks, ignore
  Segment *seg = segmentAt(idx);
  if (!seg) return MB_EX_ILLEGAL_ADDRESS;
  if (addr < MB_CO_SEG_REVERSE) seg->setOption(SEG_OPTION_ON, val);
  else if (addr < MB_CO_SEG_MIRROR) { seg->reverse = val; stateChanged = true; }
  else { seg->mirror = val; stateChanged = true; }
  changed = true;
  return 0;
}

bool ModbusTCPUsermod::readDiscrete(uint16_t addr, bool &val) {
  val = false;
  if (addr >= MB_CO_SEG_ON + MB_MAX_SEGMENTS) return false;
  if (addr >= MB_CO_SEG_ON) {
    Segment *seg = segmentAt(addr - MB_CO_SEG_ON);
    val = seg && seg->isActive();
    return true;
  }
  switch (addr) {
    case 0: val = bri > 0; break;
    case 1: val = nightlightActive; break;
    case 2: val = realtimeMode != REALTIME_MODE_INACTIVE; break;
    case 3: val = currentPlaylist >= 0; break;
    case 4: val = WLED_CONNECTED; break;
  }
  return true;
}


static ModbusTCPUsermod modbus_tcp_usermod;
REGISTER_USERMOD(modbus_tcp_usermod);
