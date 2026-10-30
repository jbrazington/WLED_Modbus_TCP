#include "wled.h"

/*
 * EtherNet/IP PCCC server usermod
 *
 * Lets an Allen-Bradley PLC (ControlLogix/CompactLogix, MicroLogix 1100/1400,
 * SLC 5/05, PLC-5/E) or an HMI control WLED with ordinary MSG instructions:
 * PLC-5 typed read/write, PLC-5 word range read/write, SLC typed read/write
 * and bit (masked) writes, carried as PCCC inside CIP "Execute PCCC" over
 * EtherNet/IP (TCP 44818), connected or unconnected.
 *
 * WLED state is presented as data-table files (numbers are configurable):
 *   N7       control: power, brightness, preset, transition, nightlight, main segment
 *   N8       status, read only: LED count, effect/palette counts, FPS, current, uptime
 *   N10-N41  segment 0-31: on, opacity, effect, speed, intensity, palette, RGBW colors
 *   B3       bits: power, nightlight, status flags, segment on/off
 *
 * The server is polled from loop() (no async callbacks), so all state changes
 * happen on the main WLED task. Works over WiFi or Ethernet. See readme.md for
 * the full data-table map.
 */

#ifndef EIP_PCCC_PORT
  #define EIP_PCCC_PORT 44818
#endif
#ifndef EIP_PCCC_MAX_CLIENTS
  #define EIP_PCCC_MAX_CLIENTS 4
#endif
#ifndef EIP_PCCC_MAX_CONNECTIONS
  #define EIP_PCCC_MAX_CONNECTIONS 8  // CIP class 3 (connected) message connections
#endif
#ifndef EIP_PCCC_VENDOR_ID
  #define EIP_PCCC_VENDOR_ID 0        // reported by List Identity / Identity object
#endif

#define EIP_HDR_LEN       24          // encapsulation header
#define EIP_FRAME_SIZE    600         // header + data; PCCC packets are under 256 bytes
#define EIP_CLOSE         0xFFFF      // processEncap(): drop the client
#define EIP_MAX_SEGMENTS  32

// encapsulation commands
#define EIP_CMD_NOP               0x0000
#define EIP_CMD_LIST_SERVICES     0x0004
#define EIP_CMD_LIST_IDENTITY     0x0063
#define EIP_CMD_LIST_INTERFACES   0x0064
#define EIP_CMD_REGISTER_SESSION  0x0065
#define EIP_CMD_UNREGISTER        0x0066
#define EIP_CMD_SEND_RR_DATA      0x006F
#define EIP_CMD_SEND_UNIT_DATA    0x0070

// encapsulation status
#define EIP_ERR_COMMAND           0x0001
#define EIP_ERR_DATA              0x0003
#define EIP_ERR_SESSION           0x0064
#define EIP_ERR_LENGTH            0x0065
#define EIP_ERR_PROTOCOL          0x0069

// common packet format item types
#define CPF_NULL                  0x0000
#define CPF_LIST_IDENTITY         0x000C
#define CPF_CONNECTED_ADDR        0x00A1
#define CPF_CONNECTED_DATA        0x00B1
#define CPF_UNCONNECTED_DATA      0x00B2
#define CPF_LIST_SERVICES         0x0100

// CIP
#define CIP_CLASS_IDENTITY        0x01
#define CIP_CLASS_CONN_MANAGER    0x06
#define CIP_CLASS_PCCC            0x67
#define CIP_SVC_GET_ALL           0x01
#define CIP_SVC_GET_SINGLE        0x0E
#define CIP_SVC_EXECUTE_PCCC      0x4B
#define CIP_SVC_FORWARD_CLOSE     0x4E
#define CIP_SVC_UNCONNECTED_SEND  0x52
#define CIP_SVC_FORWARD_OPEN      0x54
#define CIP_SVC_LARGE_FWD_OPEN    0x5B
#define CIP_ERR_CONNECTION        0x01
#define CIP_ERR_PATH_SEGMENT      0x04
#define CIP_ERR_PATH_DEST         0x05
#define CIP_ERR_SERVICE           0x08
#define CIP_ERR_NOT_ENOUGH_DATA   0x13
#define CIP_ERR_ATTRIBUTE         0x14
#define CIP_EXT_TRANSPORT         0x0103  // transport class not supported
#define CIP_EXT_NOT_FOUND         0x0107  // target connection not found
#define CIP_EXT_NO_CONNECTIONS    0x0113  // out of connections

// PCCC errors: high byte STS, low byte EXT STS (when STS is 0xF0)
#define PCCC_ERR_COMMAND          0x1000  // illegal command or format
#define PCCC_ERR_VALUE            0x1000  // value out of range for that element
#define PCCC_ERR_PROTECT          0x6000  // read only enabled in the usermod settings
#define PCCC_ERR_ADDRESS          0xF006  // address doesn't point to something usable
#define PCCC_ERR_SIZE             0xF007  // file is wrong size (past the end of the file)
#define PCCC_ERR_ACCESS           0xF00B  // access denied (write to the status file)
#define PCCC_ERR_DECODE           0xF00E  // command could not be decoded
#define PCCC_MAX_DATA             240     // bytes of data in one reply
#define PCCC_MAX_TYPED_WORDS      116     // PLC-5 typed read reply also carries type bytes

// PCCC data types
#define PCCC_FILE_BIT             0x85    // SLC file type codes
#define PCCC_FILE_INTEGER         0x89
#define PCCC_DT_BIT_STRING        0x02    // typed data (PLC-5 typed read/write)
#define PCCC_DT_INTEGER           0x04
#define PCCC_DT_ARRAY             0x09

// default data-table file numbers
#define DT_CONTROL_FILE           7
#define DT_STATUS_FILE            8
#define DT_SEGMENT_FILE           10      // segment N is file 10 + N
#define DT_BIT_FILE               3

// file lengths in elements (words)
#define DT_CONTROL_LEN            16
#define DT_STATUS_LEN             16
#define DT_SEGMENT_LEN            25
#define DT_BIT_LEN                3

// control file (N7) elements
#define DT_POWER                  0
#define DT_BRIGHTNESS             1
#define DT_PRESET                 2
#define DT_TRANSITION             3
#define DT_NL_DURATION            4
#define DT_NL_TARGET_BRI          5
#define DT_MAIN_SEGMENT           6

// segment file (N10+) elements, same layout as the Modbus usermod's segment blocks
#define DT_SEG_ON                 0
#define DT_SEG_OPACITY            1
#define DT_SEG_EFFECT             2
#define DT_SEG_SPEED              3
#define DT_SEG_INTENSITY          4
#define DT_SEG_PALETTE            5
#define DT_SEG_CUSTOM1            6
#define DT_SEG_CUSTOM2            7
#define DT_SEG_CUSTOM3            8
#define DT_SEG_COLOR              10  // 10..21: 3 colors x R,G,B,W
#define DT_SEG_CCT                22
#define DT_SEG_REVERSE            23
#define DT_SEG_MIRROR             24

// bit file (B3) word 0
#define DT_BIT_POWER              0x01
#define DT_BIT_NIGHTLIGHT         0x02
#define DT_BIT_LIVE               0x04  // read only
#define DT_BIT_PLAYLIST           0x08  // read only
#define DT_BIT_WRITABLE           (DT_BIT_POWER | DT_BIT_NIGHTLIGHT)

static inline uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

class EipPcccUsermod : public Usermod {

  private:

    struct Client {
      WiFiClient sock;
      uint8_t buf[EIP_FRAME_SIZE];
      uint16_t len = 0;
      uint32_t session = 0;           // 0 = no session registered
      unsigned long lastSeen = 0;
    };

    struct Connection {
      bool used = false;
      uint8_t client = 0;
      uint32_t otId = 0;              // ours, the PLC sends with it
      uint32_t toId = 0;              // the PLC's, we reply with it
      uint16_t serial = 0;            // connection triad, identifies it for Forward Close
      uint16_t vendor = 0;
      uint32_t origSerial = 0;
      uint32_t timeoutMs = 0;
      unsigned long lastSeen = 0;
    };

    enum FileKind : uint8_t { FK_CONTROL, FK_STATUS, FK_SEGMENT, FK_BITS };
    struct FileRef {
      FileKind kind;
      uint8_t seg;                    // segment index for FK_SEGMENT
      uint8_t type;                   // SLC file type code
      uint16_t len;                   // elements
    };

    bool enabled = true;
    bool readOnly = false;
    bool discovery = true;            // answer List Identity broadcasts (RSLinx browsing) on UDP
    uint16_t port = EIP_PCCC_PORT;
    uint16_t idleTimeout = 120;       // seconds, 0 = never
    uint16_t controlFile = DT_CONTROL_FILE;
    uint16_t statusFile = DT_STATUS_FILE;
    uint16_t segmentFile = DT_SEGMENT_FILE;
    uint16_t bitFile = DT_BIT_FILE;
    bool initDone = false;

    WiFiServer *server = nullptr;
    WiFiUDP udp;
    bool udpActive = false;
    uint16_t serverPort = 0;
    Client clients[EIP_PCCC_MAX_CLIENTS];
    Connection conns[EIP_PCCC_MAX_CONNECTIONS];
    uint8_t txBuf[EIP_FRAME_SIZE];
    uint32_t requestCount = 0;
    uint32_t idSeed = 0;
    uint32_t serialNumber = 0;

    bool changed = false;             // a write changed WLED state, call stateUpdated()
    uint16_t lastPresetWritten = 0;   // preset element is edge triggered, see writeControl()

    static const char _name[];
    static const char _enabled[];
    static const char _readOnly[];
    static const char _port[];
    static const char _idleTimeout[];
    static const char _discovery[];
    static const char _controlFile[];
    static const char _statusFile[];
    static const char _segmentFile[];
    static const char _bitFile[];

    void startServer();
    void stopServer();
    void dropClient(uint8_t idx);
    void serviceClient(uint8_t idx);
    void serviceUdp();
    uint32_t newId();

    // EtherNet/IP encapsulation and CIP, each returns the response length
    uint16_t processEncap(uint8_t idx, const uint8_t *req, uint16_t len, uint8_t *out);
    uint16_t sendRRData(uint8_t idx, const uint8_t *data, uint16_t len, uint8_t *out);
    uint16_t sendUnitData(uint8_t idx, const uint8_t *data, uint16_t len, uint8_t *out);
    uint16_t processCip(uint8_t idx, const uint8_t *req, uint16_t len, uint8_t *out, uint8_t depth);
    uint16_t forwardOpen(uint8_t idx, uint8_t svc, const uint8_t *data, uint16_t len, uint8_t *out);
    uint16_t forwardClose(uint8_t idx, const uint8_t *data, uint16_t len, uint8_t *out);
    uint16_t executePccc(const uint8_t *data, uint16_t len, uint8_t *out);
    uint16_t listIdentity(uint8_t *out);
    uint16_t listServices(uint8_t *out);
    uint16_t identityAttributes(uint8_t *out, uint8_t attr);   // attr 0 = all
    static uint16_t cipError(uint8_t *out, uint8_t svc, uint8_t status);
    static uint16_t connectionError(uint8_t *out, uint8_t svc, uint16_t ext, const uint8_t *triad);

    // PCCC, errors are PCCC_ERR_* or 0
    uint16_t processPccc(const uint8_t *req, uint16_t len, uint8_t *out);
    uint16_t typedCommand(uint8_t fnc, const uint8_t *p, const uint8_t *end, uint8_t *out, uint16_t &n);
    uint16_t parsePlc5Address(const uint8_t *&p, const uint8_t *end, uint16_t &file, uint16_t &elem);

    // data table access
    bool resolveFile(uint16_t file, FileRef &f);
    uint16_t openFile(uint16_t file, uint8_t type, uint32_t start, uint32_t count, FileRef &f);
    uint16_t checkWritable(const FileRef &f);
    void readWords(const FileRef &f, uint16_t start, uint16_t count, uint8_t *dst);
    uint16_t writeWords(const FileRef &f, uint16_t start, uint16_t count, const uint8_t *src);
    uint16_t maskWord(const FileRef &f, uint16_t elem, uint16_t andMask, uint16_t orMask);
    uint16_t readWord(const FileRef &f, uint16_t elem);
    uint16_t writeWord(const FileRef &f, uint16_t elem, uint16_t val);
    uint16_t writeControl(uint16_t elem, uint16_t val);
    uint16_t writeSegment(Segment *seg, uint16_t elem, uint16_t val);
    void setPower(bool on);

    static Segment *segmentAt(uint16_t idx) {
      if (idx >= strip.getSegmentsNum()) return nullptr;
      return &strip.getSegment(idx);
    }

  public:

    void setup() override {
      uint8_t mac[6] = {0};
      WiFi.macAddress(mac);
      serialNumber = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | (mac[4] << 8) | mac[5];
      idSeed = serialNumber ^ millis();
      initDone = true;
    }

    void connected() override {
      if (enabled) startServer();
    }

    void loop() override {
      if (!enabled || !server) return;

      WiFiClient incoming = server->available();
      if (incoming) {
        int slot = -1;
        for (int i = 0; i < EIP_PCCC_MAX_CLIENTS; i++) {
          if (!clients[i].sock || !clients[i].sock.connected()) { slot = i; break; }
        }
        if (slot >= 0) {
          dropClient(slot);
          clients[slot].sock = incoming;
          clients[slot].sock.setNoDelay(true);
          clients[slot].lastSeen = millis();
          DEBUG_PRINTF_P(PSTR("EIP: client %d connected\n"), slot);
        } else {
          incoming.stop(); // no free slot
        }
      }

      for (int i = 0; i < EIP_PCCC_MAX_CLIENTS; i++) serviceClient(i);
      if (udpActive) serviceUdp();

      // close connected-messaging connections the PLC stopped using
      unsigned long now = millis();
      for (int i = 0; i < EIP_PCCC_MAX_CONNECTIONS; i++) {
        if (conns[i].used && now - conns[i].lastSeen > conns[i].timeoutMs) conns[i].used = false;
      }

      if (changed) {
        changed = false;
        stateUpdated(CALL_MODE_DIRECT_CHANGE);
      }
    }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray info = user.createNestedArray(F("EtherNet/IP PCCC"));
      if (!enabled) { info.add(F("disabled")); return; }
      if (!server)  { info.add(F("waiting for network")); return; }
      unsigned n = 0, c = 0;
      for (int i = 0; i < EIP_PCCC_MAX_CLIENTS; i++) if (clients[i].sock && clients[i].sock.connected()) n++;
      for (int i = 0; i < EIP_PCCC_MAX_CONNECTIONS; i++) if (conns[i].used) c++;
      char buf[64];
      snprintf_P(buf, sizeof(buf), PSTR("port %u, %u client(s), %u conn, %u req"), serverPort, n, c, (unsigned)requestCount);
      info.add(buf);
    }

    void addToConfig(JsonObject &root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)]     = enabled;
      top[FPSTR(_port)]        = port;
      top[FPSTR(_readOnly)]    = readOnly;
      top[FPSTR(_idleTimeout)] = idleTimeout;
      top[FPSTR(_discovery)]   = discovery;
      top[FPSTR(_controlFile)] = controlFile;
      top[FPSTR(_statusFile)]  = statusFile;
      top[FPSTR(_segmentFile)] = segmentFile;
      top[FPSTR(_bitFile)]     = bitFile;
    }

    bool readFromConfig(JsonObject &root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled, true);
      configComplete &= getJsonValue(top[FPSTR(_port)], port, (uint16_t)EIP_PCCC_PORT);
      configComplete &= getJsonValue(top[FPSTR(_readOnly)], readOnly, false);
      configComplete &= getJsonValue(top[FPSTR(_idleTimeout)], idleTimeout, (uint16_t)120);
      configComplete &= getJsonValue(top[FPSTR(_discovery)], discovery, true);
      configComplete &= getJsonValue(top[FPSTR(_controlFile)], controlFile, (uint16_t)DT_CONTROL_FILE);
      configComplete &= getJsonValue(top[FPSTR(_statusFile)], statusFile, (uint16_t)DT_STATUS_FILE);
      configComplete &= getJsonValue(top[FPSTR(_segmentFile)], segmentFile, (uint16_t)DT_SEGMENT_FILE);
      configComplete &= getJsonValue(top[FPSTR(_bitFile)], bitFile, (uint16_t)DT_BIT_FILE);
      if (port == 0) port = EIP_PCCC_PORT;
      if (controlFile > 999) controlFile = DT_CONTROL_FILE;
      if (statusFile > 999) statusFile = DT_STATUS_FILE;
      if (segmentFile > 999 - EIP_MAX_SEGMENTS) segmentFile = DT_SEGMENT_FILE;
      if (bitFile > 999) bitFile = DT_BIT_FILE;

      // apply changes made on the settings page
      if (initDone) {
        if (!enabled) stopServer();
        else if (WLED_CONNECTED && (!server || serverPort != port || udpActive != discovery)) { stopServer(); startServer(); }
      }
      return configComplete;
    }

    void appendConfigData() override {
      oappend(F("addInfo('EIP_PCCC:port',1,'(44818 = EtherNet/IP)');"));
      oappend(F("addInfo('EIP_PCCC:readOnly',1,'(reject all writes)');"));
      oappend(F("addInfo('EIP_PCCC:idleTimeout',1,'seconds (0 = never)');"));
      oappend(F("addInfo('EIP_PCCC:discovery',1,'(answer RSLinx browse)');"));
      oappend(F("addInfo('EIP_PCCC:controlFile',1,'N file: power, brightness, preset');"));
      oappend(F("addInfo('EIP_PCCC:statusFile',1,'N file, read only');"));
      oappend(F("addInfo('EIP_PCCC:segmentFile',1,'N file of segment 0, segment n = this + n');"));
      oappend(F("addInfo('EIP_PCCC:bitFile',1,'B file');"));
    }
};

const char EipPcccUsermod::_name[]        PROGMEM = "EIP_PCCC";
const char EipPcccUsermod::_enabled[]     PROGMEM = "enabled";
const char EipPcccUsermod::_readOnly[]    PROGMEM = "readOnly";
const char EipPcccUsermod::_port[]        PROGMEM = "port";
const char EipPcccUsermod::_idleTimeout[] PROGMEM = "idleTimeout";
const char EipPcccUsermod::_discovery[]   PROGMEM = "discovery";
const char EipPcccUsermod::_controlFile[] PROGMEM = "controlFile";
const char EipPcccUsermod::_statusFile[]  PROGMEM = "statusFile";
const char EipPcccUsermod::_segmentFile[] PROGMEM = "segmentFile";
const char EipPcccUsermod::_bitFile[]     PROGMEM = "bitFile";


void EipPcccUsermod::startServer() {
  if (server && serverPort == port) return;
  stopServer();
  server = new WiFiServer(port);
  server->begin();
  server->setNoDelay(true);
  serverPort = port;
  if (discovery) udpActive = udp.begin(port);
  DEBUG_PRINTF_P(PSTR("EIP: listening on port %u\n"), port);
}

void EipPcccUsermod::stopServer() {
  for (int i = 0; i < EIP_PCCC_MAX_CLIENTS; i++) dropClient(i);
  if (server) {
    server->end();
    delete server;
    server = nullptr;
  }
  if (udpActive) udp.stop();
  udpActive = false;
  serverPort = 0;
}

void EipPcccUsermod::dropClient(uint8_t idx) {
  Client &c = clients[idx];
  c.sock.stop();
  c.len = 0;
  c.session = 0;
  for (int i = 0; i < EIP_PCCC_MAX_CONNECTIONS; i++) if (conns[i].client == idx) conns[i].used = false;
}

uint32_t EipPcccUsermod::newId() {
  idSeed = idSeed * 1664525UL + 1013904223UL + millis();
  return idSeed ? idSeed : 1;
}

void EipPcccUsermod::serviceClient(uint8_t idx) {
  Client &c = clients[idx];
  if (!c.sock) return;
  if (!c.sock.connected()) { dropClient(idx); return; }

  unsigned long now = millis();
  int avail = c.sock.available();
  if (avail <= 0) {
    if (idleTimeout && now - c.lastSeen > idleTimeout * 1000UL) dropClient(idx);
    return;
  }
  c.lastSeen = now;

  // read what fits, then handle every complete frame in the buffer
  int n = c.sock.read(c.buf + c.len, min((int)(EIP_FRAME_SIZE - c.len), avail));
  if (n > 0) c.len += n;

  while (c.len >= EIP_HDR_LEN) {
    uint16_t frameLen = EIP_HDR_LEN + le16(c.buf + 2);
    if (frameLen > EIP_FRAME_SIZE) { dropClient(idx); return; } // nothing we serve is that big
    if (c.len < frameLen) break; // wait for the rest

    uint16_t respLen = processEncap(idx, c.buf, frameLen, txBuf);
    requestCount++;
    if (respLen == EIP_CLOSE) { dropClient(idx); return; }
    if (respLen) c.sock.write(txBuf, respLen);

    memmove(c.buf, c.buf + frameLen, c.len - frameLen);
    c.len -= frameLen;
  }
}

// List Identity over UDP, which is how RSLinx and other tools browse a network
void EipPcccUsermod::serviceUdp() {
  int size = udp.parsePacket();
  if (size < EIP_HDR_LEN) return;
  uint8_t req[EIP_HDR_LEN];
  if (udp.read(req, EIP_HDR_LEN) != EIP_HDR_LEN || le16(req) != EIP_CMD_LIST_IDENTITY) return;
  memcpy(txBuf, req, EIP_HDR_LEN);
  put32(txBuf + 8, 0);
  uint16_t n = listIdentity(txBuf + EIP_HDR_LEN);
  put16(txBuf + 2, n);
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.write(txBuf, EIP_HDR_LEN + n);
  udp.endPacket();
}

/*
 * Handles one encapsulation frame. Returns the response length, 0 for no
 * response or EIP_CLOSE to drop the client.
 */
uint16_t EipPcccUsermod::processEncap(uint8_t idx, const uint8_t *req, uint16_t len, uint8_t *out) {
  Client &c = clients[idx];
  uint16_t cmd = le16(req);
  const uint8_t *data = req + EIP_HDR_LEN;
  uint16_t dataLen = len - EIP_HDR_LEN;
  uint8_t *rdata = out + EIP_HDR_LEN;
  uint16_t rlen = 0;
  uint32_t status = 0;

  memcpy(out, req, EIP_HDR_LEN); // command, session handle, sender context and options are echoed

  switch (cmd) {
    case EIP_CMD_NOP:
      return 0;
    case EIP_CMD_LIST_SERVICES:
      rlen = listServices(rdata);
      break;
    case EIP_CMD_LIST_IDENTITY:
      rlen = listIdentity(rdata);
      break;
    case EIP_CMD_LIST_INTERFACES:
      put16(rdata, 0); // no items
      rlen = 2;
      break;
    case EIP_CMD_REGISTER_SESSION:
      if (dataLen != 4) { status = EIP_ERR_LENGTH; break; }
      memcpy(rdata, data, 4);
      rlen = 4;
      if (le16(data) != 1) { status = EIP_ERR_PROTOCOL; put16(rdata, 1); break; } // we speak version 1
      if (!c.session) c.session = newId();
      put32(out + 4, c.session);
      break;
    case EIP_CMD_UNREGISTER:
      return EIP_CLOSE;
    case EIP_CMD_SEND_RR_DATA:
    case EIP_CMD_SEND_UNIT_DATA:
      if (!c.session || le32(req + 4) != c.session) { status = EIP_ERR_SESSION; break; }
      if (cmd == EIP_CMD_SEND_RR_DATA) {
        rlen = sendRRData(idx, data, dataLen, rdata);
        if (!rlen) status = EIP_ERR_DATA;
      } else {
        rlen = sendUnitData(idx, data, dataLen, rdata);
        if (!rlen) return 0; // unknown connection: no reply, as the spec says
      }
      break;
    default:
      status = EIP_ERR_COMMAND;
  }

  put32(out + 8, status);
  put16(out + 2, rlen);
  return EIP_HDR_LEN + rlen;
}

// unconnected message: interface handle, timeout, CPF { null address, unconnected data }
uint16_t EipPcccUsermod::sendRRData(uint8_t idx, const uint8_t *data, uint16_t len, uint8_t *out) {
  if (len < 8) return 0;
  const uint8_t *p = data + 8, *end = data + len;
  const uint8_t *cip = nullptr;
  uint16_t cipLen = 0;
  for (uint16_t items = le16(data + 6); items; items--) {
    if (end - p < 4) return 0;
    uint16_t type = le16(p), itemLen = le16(p + 2);
    p += 4;
    if (end - p < itemLen) return 0;
    if (type == CPF_UNCONNECTED_DATA) { cip = p; cipLen = itemLen; }
    p += itemLen;
  }
  if (!cip) return 0;

  put32(out, 0);      // interface handle
  put16(out + 4, 0);  // timeout
  put16(out + 6, 2);  // item count
  put16(out + 8, CPF_NULL);
  put16(out + 10, 0);
  put16(out + 12, CPF_UNCONNECTED_DATA);
  uint16_t n = processCip(idx, cip, cipLen, out + 16, 0);
  put16(out + 14, n);
  return 16 + n;
}

// connected message: CPF { connected address, connected data (sequence + CIP) }
uint16_t EipPcccUsermod::sendUnitData(uint8_t idx, const uint8_t *data, uint16_t len, uint8_t *out) {
  if (len < 8) return 0;
  const uint8_t *p = data + 8, *end = data + len;
  const uint8_t *cip = nullptr;
  uint16_t cipLen = 0;
  uint32_t connId = 0;
  for (uint16_t items = le16(data + 6); items; items--) {
    if (end - p < 4) return 0;
    uint16_t type = le16(p), itemLen = le16(p + 2);
    p += 4;
    if (end - p < itemLen) return 0;
    if (type == CPF_CONNECTED_ADDR && itemLen == 4) connId = le32(p);
    else if (type == CPF_CONNECTED_DATA && itemLen >= 2) { cip = p; cipLen = itemLen; }
    p += itemLen;
  }
  if (!cip) return 0;

  Connection *cn = nullptr;
  for (int i = 0; i < EIP_PCCC_MAX_CONNECTIONS; i++) {
    if (conns[i].used && conns[i].client == idx && conns[i].otId == connId) { cn = &conns[i]; break; }
  }
  if (!cn) return 0;
  cn->lastSeen = millis();

  put32(out, 0);
  put16(out + 4, 0);
  put16(out + 6, 2);
  put16(out + 8, CPF_CONNECTED_ADDR);
  put16(out + 10, 4);
  put32(out + 12, cn->toId);
  put16(out + 16, CPF_CONNECTED_DATA);
  memcpy(out + 20, cip, 2); // sequence count is echoed
  uint16_t n = processCip(idx, cip + 2, cipLen - 2, out + 22, 0);
  put16(out + 18, 2 + n);
  return 22 + n;
}

uint16_t EipPcccUsermod::cipError(uint8_t *out, uint8_t svc, uint8_t status) {
  out[0] = svc | 0x80;
  out[1] = 0;
  out[2] = status;
  out[3] = 0;
  return 4;
}

// failed Forward Open/Close: extended status, then the connection triad and remaining path size
uint16_t EipPcccUsermod::connectionError(uint8_t *out, uint8_t svc, uint16_t ext, const uint8_t *triad) {
  out[0] = svc | 0x80;
  out[1] = 0;
  out[2] = CIP_ERR_CONNECTION;
  out[3] = 1;
  put16(out + 4, ext);
  memcpy(out + 6, triad, 8);
  out[14] = 0;
  out[15] = 0;
  return 16;
}

/*
 * Handles one CIP request (service, path, data) and writes the reply to out.
 * depth > 0 inside Unconnected Send, which is unwrapped only once.
 */
uint16_t EipPcccUsermod::processCip(uint8_t idx, const uint8_t *req, uint16_t len, uint8_t *out, uint8_t depth) {
  if (len < 2) return cipError(out, len ? req[0] : 0, CIP_ERR_NOT_ENOUGH_DATA);
  uint8_t svc = req[0];
  uint16_t pathLen = req[1] * 2;
  if (len < 2 + pathLen) return cipError(out, svc, CIP_ERR_PATH_SEGMENT);
  const uint8_t *data = req + 2 + pathLen;
  uint16_t dataLen = len - 2 - pathLen;

  // logical segments: class, instance, attribute (8 or 16 bit)
  uint16_t cls = 0, inst = 0, attr = 0;
  for (const uint8_t *p = req + 2, *end = data; p < end; ) {
    uint8_t seg = p[0];
    uint16_t *dst = nullptr;
    switch (seg & 0xFC) {
      case 0x20: dst = &cls; break;
      case 0x24: dst = &inst; break;
      case 0x30: dst = &attr; break;
      default: return cipError(out, svc, CIP_ERR_PATH_SEGMENT);
    }
    if ((seg & 0x03) == 0 && end - p >= 2) { *dst = p[1]; p += 2; }
    else if ((seg & 0x03) == 1 && end - p >= 4) { *dst = le16(p + 2); p += 4; }
    else return cipError(out, svc, CIP_ERR_PATH_SEGMENT);
  }

  switch (cls) {
    case CIP_CLASS_CONN_MANAGER:
      if (inst != 1) return cipError(out, svc, CIP_ERR_PATH_DEST);
      if (svc == CIP_SVC_UNCONNECTED_SEND && depth == 0) {
        // priority/tick, timeout ticks, message size, message, [pad], route path: we are the target
        if (dataLen < 4 || 4 + le16(data + 2) > dataLen) return cipError(out, svc, CIP_ERR_NOT_ENOUGH_DATA);
        return processCip(idx, data + 4, le16(data + 2), out, depth + 1);
      }
      if (svc == CIP_SVC_FORWARD_OPEN || svc == CIP_SVC_LARGE_FWD_OPEN) return forwardOpen(idx, svc, data, dataLen, out);
      if (svc == CIP_SVC_FORWARD_CLOSE) return forwardClose(idx, data, dataLen, out);
      return cipError(out, svc, CIP_ERR_SERVICE);

    case CIP_CLASS_PCCC:
      if (svc != CIP_SVC_EXECUTE_PCCC) return cipError(out, svc, CIP_ERR_SERVICE);
      return executePccc(data, dataLen, out);

    case CIP_CLASS_IDENTITY: {
      if (inst != 1) return cipError(out, svc, CIP_ERR_PATH_DEST);
      if (svc != CIP_SVC_GET_ALL && svc != CIP_SVC_GET_SINGLE) return cipError(out, svc, CIP_ERR_SERVICE);
      uint16_t n = identityAttributes(out + 4, svc == CIP_SVC_GET_ALL ? 0 : attr);
      if (!n) return cipError(out, svc, CIP_ERR_ATTRIBUTE);
      cipError(out, svc, 0);
      return 4 + n;
    }

    default:
      return cipError(out, svc, CIP_ERR_PATH_DEST);
  }
}

uint16_t EipPcccUsermod::forwardOpen(uint8_t idx, uint8_t svc, const uint8_t *data, uint16_t len, uint8_t *out) {
  // fixed part up to and including the connection path size; Large Forward Open has 32 bit parameters
  bool large = svc == CIP_SVC_LARGE_FWD_OPEN;
  uint16_t fixed = large ? 40 : 36;
  if (len < fixed) return cipError(out, svc, CIP_ERR_NOT_ENOUGH_DATA);
  const uint8_t *triad = data + 10; // connection serial, vendor id, originator serial
  uint32_t toId = le32(data + 6);
  uint8_t multiplier = data[18] < 7 ? data[18] : 7;
  uint32_t otRpi = le32(data + 22);
  uint32_t toRpi = le32(data + (large ? 30 : 28));
  uint8_t transport = data[fixed - 2];

  // only explicit messaging (class 3); I/O connections are not supported
  if ((transport & 0x0F) != 3) return connectionError(out, svc, CIP_EXT_TRANSPORT, triad);

  Connection *cn = nullptr;
  for (int i = 0; i < EIP_PCCC_MAX_CONNECTIONS && !cn; i++) { // reopening the same connection replaces it
    if (conns[i].used && conns[i].serial == le16(triad) && conns[i].vendor == le16(triad + 2) && conns[i].origSerial == le32(triad + 4)) cn = &conns[i];
  }
  for (int i = 0; i < EIP_PCCC_MAX_CONNECTIONS && !cn; i++) if (!conns[i].used) cn = &conns[i];
  if (!cn) return connectionError(out, svc, CIP_EXT_NO_CONNECTIONS, triad);

  cn->used = true;
  cn->client = idx;
  cn->otId = newId();
  cn->toId = toId;
  cn->serial = le16(triad);
  cn->vendor = le16(triad + 2);
  cn->origSerial = le32(triad + 4);
  cn->timeoutMs = max(otRpi / 1000UL * (4UL << multiplier), 1000UL);
  cn->lastSeen = millis();

  cipError(out, svc, 0);
  put32(out + 4, cn->otId);
  put32(out + 8, cn->toId);
  memcpy(out + 12, triad, 8);
  put32(out + 20, otRpi);  // actual packet intervals = requested
  put32(out + 24, toRpi);
  out[28] = 0;             // application reply size
  out[29] = 0;
  return 30;
}

uint16_t EipPcccUsermod::forwardClose(uint8_t idx, const uint8_t *data, uint16_t len, uint8_t *out) {
  if (len < 10) return cipError(out, CIP_SVC_FORWARD_CLOSE, CIP_ERR_NOT_ENOUGH_DATA);
  const uint8_t *triad = data + 2;
  for (int i = 0; i < EIP_PCCC_MAX_CONNECTIONS; i++) {
    Connection &cn = conns[i];
    if (cn.used && cn.serial == le16(triad) && cn.vendor == le16(triad + 2) && cn.origSerial == le32(triad + 4)) {
      cn.used = false;
      cipError(out, CIP_SVC_FORWARD_CLOSE, 0);
      memcpy(out + 4, triad, 8);
      out[12] = 0; // application reply size
      out[13] = 0;
      return 14;
    }
  }
  return connectionError(out, CIP_SVC_FORWARD_CLOSE, CIP_EXT_NOT_FOUND, triad);
}

// Execute PCCC: requestor id (length, vendor, serial, ...), then the PCCC command
uint16_t EipPcccUsermod::executePccc(const uint8_t *data, uint16_t len, uint8_t *out) {
  if (len < 1 || data[0] < 1 || data[0] + 4 > len) return cipError(out, CIP_SVC_EXECUTE_PCCC, CIP_ERR_NOT_ENOUGH_DATA);
  uint8_t idLen = data[0];
  cipError(out, CIP_SVC_EXECUTE_PCCC, 0);
  memcpy(out + 4, data, idLen); // the reply starts with the same requestor id
  return 4 + idLen + processPccc(data + idLen, len - idLen, out + 4 + idLen);
}

uint16_t EipPcccUsermod::identityAttributes(uint8_t *out, uint8_t attr) {
  uint8_t *p = out;
  if (!attr || attr == 1) { put16(p, EIP_PCCC_VENDOR_ID); p += 2; }
  if (!attr || attr == 2) { put16(p, 0x2B); p += 2; }        // device type: generic device
  if (!attr || attr == 3) { put16(p, 1); p += 2; }           // product code
  if (!attr || attr == 4) { p[0] = 1; p[1] = 0; p += 2; }    // revision 1.0
  if (!attr || attr == 5) { put16(p, 0); p += 2; }           // status
  if (!attr || attr == 6) { put32(p, serialNumber); p += 4; }
  if (!attr || attr == 7) {
    size_t n = strlen(serverDescription);
    if (n > 32) n = 32;
    *p++ = n;
    memcpy(p, serverDescription, n);
    p += n;
  }
  return p - out;
}

uint16_t EipPcccUsermod::listIdentity(uint8_t *out) {
  put16(out, 1); // item count
  put16(out + 2, CPF_LIST_IDENTITY);
  uint8_t *p = out + 6;
  put16(p, 1);   // encapsulation protocol version
  // socket address, big endian
  IPAddress ip = Network.localIP();
  p[2] = 0; p[3] = 2; // AF_INET
  p[4] = serverPort >> 8; p[5] = serverPort & 0xFF;
  for (int i = 0; i < 4; i++) p[6 + i] = ip[i];
  memset(p + 10, 0, 8);
  uint16_t n = 18 + identityAttributes(p + 18, 0);
  p[n++] = 3;    // state: operational
  put16(out + 4, n);
  return 6 + n;
}

uint16_t EipPcccUsermod::listServices(uint8_t *out) {
  put16(out, 1);
  put16(out + 2, CPF_LIST_SERVICES);
  put16(out + 4, 20);
  put16(out + 6, 1);       // version
  put16(out + 8, 0x0020);  // supports CIP over TCP, no class 0/1 UDP
  memset(out + 10, 0, 16);
  memcpy(out + 10, "Communications", 14);
  return 26;
}


/*
 * Handles one PCCC command (CMD, STS, TNS, FNC, data) and writes the reply
 * (CMD | 0x40, STS, TNS, [EXT STS], data). Returns its length.
 */
uint16_t EipPcccUsermod::processPccc(const uint8_t *req, uint16_t len, uint8_t *out) {
  uint8_t cmd = req[0];
  out[0] = cmd | 0x40;
  out[1] = 0;
  out[2] = req[2]; // TNS is echoed
  out[3] = req[3];
  uint16_t n = 4;
  uint16_t err = PCCC_ERR_COMMAND;

  if (len >= 5) {
    const uint8_t *p = req + 5, *end = req + len;
    if (cmd == 0x06 && req[4] == 0x00) {         // echo
      n += min((int)(end - p), PCCC_MAX_DATA);
      memcpy(out + 4, p, n - 4);
      err = 0;
    } else if (cmd == 0x0F) {
      err = typedCommand(req[4], p, end, out, n);
    }
  }

  if (!err) return n;
  if ((err >> 8) == 0xF0) {
    out[1] = 0xF0;
    out[4] = err & 0xFF;
    return 5;
  }
  out[1] = err >> 8;
  return 4;
}

// address field in SLC addresses and PLC-5 logical binary levels: 1 byte, or 0xFF + 16 bit
static bool getField(const uint8_t *&p, const uint8_t *end, uint16_t &v) {
  if (p >= end) return false;
  if (*p != 0xFF) { v = *p++; return true; }
  if (end - p < 3) return false;
  v = le16(p + 1);
  p += 3;
  return true;
}

// PCCC typed data descriptor: type and size nibbles, either can be extended with following bytes
static bool getDescriptor(const uint8_t *&p, const uint8_t *end, uint16_t &type, uint16_t &size) {
  if (p >= end) return false;
  uint8_t d = *p++;
  uint8_t t = d >> 4, s = d & 0x0F;
  type = t; size = s;
  if (t & 0x08) {
    t &= 0x07;
    if (t < 1 || t > 2 || end - p < t) return false;
    type = t == 1 ? p[0] : le16(p);
    p += t;
  }
  if (s & 0x08) {
    s &= 0x07;
    if (s < 1 || s > 2 || end - p < s) return false;
    size = s == 1 ? p[0] : le16(p);
    p += s;
  }
  return true;
}

/*
 * PLC-5 system address: logical binary (level mask, then data table, file,
 * element, sub-element levels) or logical ASCII ("\0$N7:0\0").
 */
uint16_t EipPcccUsermod::parsePlc5Address(const uint8_t *&p, const uint8_t *end, uint16_t &file, uint16_t &elem) {
  file = elem = 0;
  if (p >= end) return PCCC_ERR_DECODE;

  if (*p == 0 && end - p > 1 && p[1] == '$') {
    p += 2;
    while (p < end && *p >= 'A' && *p <= 'Z') p++; // file type letter(s), the number decides
    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') { file = file * 10 + (*p++ - '0'); digits = true; }
    if (!digits || p >= end || *p++ != ':') return PCCC_ERR_ADDRESS;
    digits = false;
    while (p < end && *p >= '0' && *p <= '9') { elem = elem * 10 + (*p++ - '0'); digits = true; }
    if (!digits || p >= end || *p++ != 0) return PCCC_ERR_ADDRESS; // bit or sub-element addresses not supported
    return 0;
  }

  uint8_t mask = *p++;
  uint16_t level[4] = {0, 0, 0, 0};
  for (int i = 0; i < 8; i++) {
    if (!(mask & (1 << i))) continue;
    uint16_t v;
    if (!getField(p, end, v)) return PCCC_ERR_DECODE;
    if (i > 3) return PCCC_ERR_ADDRESS;
    level[i] = v;
  }
  // level 0 is the data table area (0), sub-elements don't exist in N and B files
  if (!(mask & 0x02) || level[0] || level[3]) return PCCC_ERR_ADDRESS;
  file = level[1];
  elem = level[2];
  return 0;
}

uint16_t EipPcccUsermod::typedCommand(uint8_t fnc, const uint8_t *p, const uint8_t *end, uint8_t *out, uint16_t &n) {
  FileRef f;
  uint16_t err;

  switch (fnc) {
    case 0x68:   // PLC-5 typed read
    case 0x01: { // PLC-5 word range read
      if (end - p < 4) return PCCC_ERR_DECODE;
      uint16_t offset = le16(p), total = le16(p + 2), file, elem, count;
      p += 4;
      if ((err = parsePlc5Address(p, end, file, elem))) return err;
      if (fnc == 0x68) count = end - p >= 2 ? le16(p) : total;   // elements
      else count = end - p >= 1 ? *p / 2 : total;                // trailing size is in bytes
      if (count > (fnc == 0x68 ? PCCC_MAX_TYPED_WORDS : PCCC_MAX_DATA / 2)) return PCCC_ERR_SIZE;
      if ((err = openFile(file, 0, (uint32_t)elem + offset, count, f))) return err;
      if (fnc == 0x68) {
        // array descriptor (type 9, size = element descriptor + data), then the element descriptor
        uint16_t size = 1 + 2 * count;
        if (size <= 7) { out[n++] = 0x90 | size; out[n++] = PCCC_DT_ARRAY; }
        else { out[n++] = 0x99; out[n++] = PCCC_DT_ARRAY; out[n++] = size; }
        out[n++] = ((f.kind == FK_BITS ? PCCC_DT_BIT_STRING : PCCC_DT_INTEGER) << 4) | 2;
      }
      readWords(f, elem + offset, count, out + n);
      n += 2 * count;
      return 0;
    }

    case 0x67:   // PLC-5 typed write
    case 0x00: { // PLC-5 word range write
      if (end - p < 4) return PCCC_ERR_DECODE;
      uint16_t offset = le16(p), file, elem;
      p += 4;
      if ((err = parsePlc5Address(p, end, file, elem))) return err;
      if (fnc == 0x67) {
        uint16_t type, size;
        if (!getDescriptor(p, end, type, size)) return PCCC_ERR_DECODE;
        if (type == PCCC_DT_ARRAY && !getDescriptor(p, end, type, size)) return PCCC_ERR_DECODE;
        if ((type != PCCC_DT_INTEGER && type != PCCC_DT_BIT_STRING) || size != 2) return PCCC_ERR_DECODE; // 16 bit words only
      }
      uint16_t count = (end - p) / 2; // an odd trailing byte is padding
      if (count > PCCC_MAX_DATA / 2) return PCCC_ERR_SIZE;
      if ((err = openFile(file, 0, (uint32_t)elem + offset, count, f))) return err;
      return writeWords(f, elem + offset, count, p);
    }

    case 0x26: { // PLC-5 read-modify-write: one or more { address, AND mask, OR mask }
      // check every address first so a bad one changes nothing
      for (int pass = 0; pass < 2; pass++) {
        for (const uint8_t *q = p; q < end; ) {
          uint16_t file, elem;
          if ((err = parsePlc5Address(q, end, file, elem))) return err;
          if (end - q < 4) return PCCC_ERR_DECODE;
          uint16_t andMask = le16(q), orMask = le16(q + 2);
          q += 4;
          if ((err = openFile(file, 0, elem, 1, f))) return err;
          if ((err = pass ? maskWord(f, elem, andMask, orMask) : checkWritable(f))) return err;
        }
      }
      return 0;
    }

    case 0xA1:   // SLC protected typed logical read, 2 address fields
    case 0xA2:   // ... 3 address fields
    case 0xA9:   // SLC protected typed logical write, 2 address fields
    case 0xAA:   // ... 3 address fields
    case 0xAB: { // ... with mask, 3 address fields (bit writes)
      if (p >= end) return PCCC_ERR_DECODE;
      uint8_t bytes = *p++;
      uint16_t file, type, elem, sub = 0;
      if (!getField(p, end, file) || !getField(p, end, type) || !getField(p, end, elem)) return PCCC_ERR_DECODE;
      if (fnc != 0xA1 && fnc != 0xA9 && !getField(p, end, sub)) return PCCC_ERR_DECODE;
      if (sub) return PCCC_ERR_ADDRESS;
      if ((bytes & 1) || bytes > PCCC_MAX_DATA) return PCCC_ERR_SIZE;
      uint16_t count = bytes / 2;
      if ((err = openFile(file, type, elem, count, f))) return err;
      if (fnc == 0xA1 || fnc == 0xA2) {
        readWords(f, elem, count, out + n);
        n += bytes;
        return 0;
      }
      if (fnc != 0xAB) {
        if (end - p < bytes) return PCCC_ERR_DECODE;
        return writeWords(f, elem, count, p);
      }
      // one 16 bit mask for all elements, then the values; only masked bits change
      if (end - p < 2 + bytes) return PCCC_ERR_DECODE;
      if ((err = checkWritable(f))) return err;
      uint16_t mask = le16(p);
      for (uint16_t i = 0; i < count; i++) {
        if ((err = maskWord(f, elem + i, ~mask, le16(p + 2 + i * 2) & mask))) return err;
      }
      return 0;
    }
  }
  return PCCC_ERR_COMMAND;
}


bool EipPcccUsermod::resolveFile(uint16_t file, FileRef &f) {
  f.seg = 0;
  if (file == controlFile)     { f.kind = FK_CONTROL; f.type = PCCC_FILE_INTEGER; f.len = DT_CONTROL_LEN; }
  else if (file == statusFile) { f.kind = FK_STATUS;  f.type = PCCC_FILE_INTEGER; f.len = DT_STATUS_LEN; }
  else if (file == bitFile)    { f.kind = FK_BITS;    f.type = PCCC_FILE_BIT;     f.len = DT_BIT_LEN; }
  else if (file >= segmentFile && file < segmentFile + EIP_MAX_SEGMENTS) {
    f.kind = FK_SEGMENT; f.type = PCCC_FILE_INTEGER; f.len = DT_SEGMENT_LEN; f.seg = file - segmentFile;
  }
  else return false;
  return true;
}

// resolves a file and checks the element range; type 0 skips the SLC file type check
uint16_t EipPcccUsermod::openFile(uint16_t file, uint8_t type, uint32_t start, uint32_t count, FileRef &f) {
  if (!resolveFile(file, f) || (type && type != f.type)) return PCCC_ERR_ADDRESS;
  if (count == 0 || start + count > f.len) return PCCC_ERR_SIZE;
  return 0;
}

uint16_t EipPcccUsermod::checkWritable(const FileRef &f) {
  if (readOnly) return PCCC_ERR_PROTECT;
  if (f.kind == FK_STATUS) return PCCC_ERR_ACCESS;
  if (f.kind == FK_SEGMENT && !segmentAt(f.seg)) return PCCC_ERR_ADDRESS; // can't write a segment that doesn't exist
  return 0;
}

void EipPcccUsermod::readWords(const FileRef &f, uint16_t start, uint16_t count, uint8_t *dst) {
  for (uint16_t i = 0; i < count; i++) put16(dst + i * 2, readWord(f, start + i));
}

// writes words in order and stops at the first value error; the range was checked by openFile()
uint16_t EipPcccUsermod::writeWords(const FileRef &f, uint16_t start, uint16_t count, const uint8_t *src) {
  uint16_t err = checkWritable(f);
  for (uint16_t i = 0; i < count && !err; i++) err = writeWord(f, start + i, le16(src + i * 2));
  return err;
}

uint16_t EipPcccUsermod::maskWord(const FileRef &f, uint16_t elem, uint16_t andMask, uint16_t orMask) {
  return writeWord(f, elem, (readWord(f, elem) & andMask) | orMask);
}

uint16_t EipPcccUsermod::readWord(const FileRef &f, uint16_t elem) {
  switch (f.kind) {
    case FK_CONTROL:
      switch (elem) {
        case DT_POWER:         return bri > 0;
        case DT_BRIGHTNESS:    return bri ? bri : briLast; // level used when on
        case DT_PRESET:        return currentPreset;
        case DT_TRANSITION:    return transitionDelay;
        case DT_NL_DURATION:   return nightlightDelayMins;
        case DT_NL_TARGET_BRI: return nightlightTargetBri;
        case DT_MAIN_SEGMENT:  return strip.getMainSegmentId();
      }
      return 0; // unused elements read as 0

    case FK_STATUS: {
      uint32_t up = millis() / 1000;
      uint32_t ver = VERSION;
      switch (elem) {
        case 0:  return strip.getLengthTotal();
        case 1:  return strip.getSegmentsNum();
        case 2:  return strip.getModeCount();
        case 3:  return getPaletteCount();
        case 4:  return strip.getMainSegmentId();
        case 5:  return strip.getFps();
        case 6:  return BusManager::currentMilliamps();
        case 7:  return BusManager::ablMilliampsMax();
        case 8:  return ESP.getFreeHeap() / 1024;
        case 9:  return (uint16_t)(int16_t)WiFi.RSSI();
        case 10: return up >> 16;
        case 11: return up & 0xFFFF;
        case 12: return ver >> 16;
        case 13: return ver & 0xFFFF;
      }
      return 0;
    }

    case FK_SEGMENT: {
      Segment *seg = segmentAt(f.seg);
      if (!seg) return 0; // missing segment reads as 0 so whole files can be polled
      if (elem >= DT_SEG_COLOR && elem < DT_SEG_COLOR + 3*4) {
        uint32_t c = seg->colors[(elem - DT_SEG_COLOR) / 4];
        switch ((elem - DT_SEG_COLOR) % 4) { case 0: return R(c); case 1: return G(c); case 2: return B(c); default: return W(c); }
      }
      switch (elem) {
        case DT_SEG_ON:        return seg->on;
        case DT_SEG_OPACITY:   return seg->opacity;
        case DT_SEG_EFFECT:    return seg->mode;
        case DT_SEG_SPEED:     return seg->speed;
        case DT_SEG_INTENSITY: return seg->intensity;
        case DT_SEG_PALETTE:   return seg->palette;
        case DT_SEG_CUSTOM1:   return seg->custom1;
        case DT_SEG_CUSTOM2:   return seg->custom2;
        case DT_SEG_CUSTOM3:   return seg->custom3;
        case DT_SEG_CCT:       return seg->cct;
        case DT_SEG_REVERSE:   return seg->reverse;
        case DT_SEG_MIRROR:    return seg->mirror;
      }
      return 0;
    }

    case FK_BITS: {
      uint16_t v = 0;
      if (elem == 0) {
        if (bri > 0) v |= DT_BIT_POWER;
        if (nightlightActive) v |= DT_BIT_NIGHTLIGHT;
        if (realtimeMode != REALTIME_MODE_INACTIVE) v |= DT_BIT_LIVE;
        if (currentPlaylist >= 0) v |= DT_BIT_PLAYLIST;
        return v;
      }
      // words 1 and 2: segment on/off, segment (elem - 1) * 16 + bit
      for (unsigned b = 0; b < 16; b++) {
        Segment *seg = segmentAt((elem - 1) * 16 + b);
        if (seg && seg->on) v |= 1 << b;
      }
      return v;
    }
  }
  return 0;
}

uint16_t EipPcccUsermod::writeWord(const FileRef &f, uint16_t elem, uint16_t val) {
  // PLCs often write their whole block every scan: writing the current value must be a
  // no-op, or every MSG restarts effects and spams sync/UI
  uint16_t cur = readWord(f, elem);
  if (cur == val && !(f.kind == FK_CONTROL && elem == DT_PRESET)) return 0;

  switch (f.kind) {
    case FK_CONTROL:
      return writeControl(elem, val);

    case FK_SEGMENT:
      return writeSegment(segmentAt(f.seg), elem, val);

    case FK_BITS: {
      uint16_t diff = cur ^ val;
      if (elem == 0) {
        if (diff & DT_BIT_POWER) setPower(val & DT_BIT_POWER);
        if (diff & DT_BIT_NIGHTLIGHT) { nightlightActive = val & DT_BIT_NIGHTLIGHT; changed = true; }
        return 0; // the other bits are status, writes to them are ignored
      }
      for (unsigned b = 0; b < 16; b++) {
        if (!(diff & (1 << b))) continue;
        Segment *seg = segmentAt((elem - 1) * 16 + b);
        if (!seg) continue; // bits of missing segments are ignored
        seg->setOption(SEG_OPTION_ON, (val >> b) & 1);
        changed = true;
      }
      return 0;
    }

    default:
      return PCCC_ERR_ACCESS;
  }
}

void EipPcccUsermod::setPower(bool on) {
  if (on != (bri > 0)) toggleOnOff();
  if (on) for (size_t s = 0; s < strip.getSegmentsNum(); s++) strip.getSegment(s).freeze = false; // like the JSON API
  changed = true;
}

uint16_t EipPcccUsermod::writeControl(uint16_t elem, uint16_t val) {
  switch (elem) {
    case DT_POWER:
      if (val > 1) return PCCC_ERR_VALUE;
      setPower(val);
      break;
    case DT_BRIGHTNESS:
      if (val > 255) return PCCC_ERR_VALUE;
      // 0 turns off; otherwise set the level without touching power, so a
      // block write of power=0 + brightness=200 stays off
      if (val == 0) { if (bri) toggleOnOff(); }
      else if (bri) bri = val;
      else briLast = val; // used by the next power on
      changed = true;
      break;
    case DT_PRESET:
      // Apply only when the written value changes. Comparing with currentPreset is not
      // enough: any other change (e.g. power off) clears currentPreset, so a PLC that
      // keeps writing the same preset would re-apply it, and with it the preset's power state.
      // Write 0 to re-arm, so the same preset can be applied again.
      if (val > 250) return PCCC_ERR_VALUE;
      if (val == lastPresetWritten) break;
      lastPresetWritten = val;
      if (val == 0) break;
      applyPreset(val, CALL_MODE_DIRECT_CHANGE); // loads asynchronously and notifies by itself
      break;
    case DT_TRANSITION:
      transitionDelay = val;
      strip.setTransition(val);
      break;
    case DT_NL_DURATION:
      if (val < 1 || val > 255) return PCCC_ERR_VALUE;
      nightlightDelayMins = val;
      break;
    case DT_NL_TARGET_BRI:
      if (val > 255) return PCCC_ERR_VALUE;
      nightlightTargetBri = val;
      break;
    case DT_MAIN_SEGMENT:
      if (val >= strip.getSegmentsNum()) return PCCC_ERR_VALUE;
      strip.setMainSegmentId(val);
      changed = true;
      break;
    default: break; // unused elements accept and ignore writes
  }
  return 0;
}

uint16_t EipPcccUsermod::writeSegment(Segment *seg, uint16_t elem, uint16_t val) {
  if (elem >= DT_SEG_COLOR && elem < DT_SEG_COLOR + 3*4) {
    if (val > 255) return PCCC_ERR_VALUE;
    unsigned slot = (elem - DT_SEG_COLOR) / 4, ch = (elem - DT_SEG_COLOR) % 4;
    uint32_t c = seg->colors[slot];
    uint8_t r = R(c), g = G(c), b = B(c), w = W(c);
    switch (ch) { case 0: r = val; break; case 1: g = val; break; case 2: b = val; break; default: w = val; }
    seg->setColor(slot, RGBW32(r, g, b, w));
    changed = true;
    return 0;
  }

  switch (elem) {
    case DT_SEG_ON:
      if (val > 1) return PCCC_ERR_VALUE;
      seg->setOption(SEG_OPTION_ON, val);
      break;
    case DT_SEG_OPACITY:
      if (val > 255) return PCCC_ERR_VALUE;
      // same rule as master brightness: 0 turns the segment off, otherwise only the level changes
      if (val > 0) seg->setOpacity(val);
      else seg->setOption(SEG_OPTION_ON, false);
      break;
    case DT_SEG_EFFECT:
      if (val >= strip.getModeCount()) return PCCC_ERR_VALUE;
      if (currentPlaylist >= 0) unloadPlaylist();
      if (val != seg->mode) seg->setMode(val);
      break;
    case DT_SEG_SPEED:
      if (val > 255) return PCCC_ERR_VALUE;
      seg->speed = val;
      stateChanged = true;
      break;
    case DT_SEG_INTENSITY:
      if (val > 255) return PCCC_ERR_VALUE;
      seg->intensity = val;
      stateChanged = true;
      break;
    case DT_SEG_PALETTE:
      if (val >= getPaletteCount()) return PCCC_ERR_VALUE;
      seg->setPalette(val);
      break;
    case DT_SEG_CUSTOM1:
      if (val > 255) return PCCC_ERR_VALUE;
      seg->custom1 = val;
      stateChanged = true;
      break;
    case DT_SEG_CUSTOM2:
      if (val > 255) return PCCC_ERR_VALUE;
      seg->custom2 = val;
      stateChanged = true;
      break;
    case DT_SEG_CUSTOM3:
      if (val > 31) return PCCC_ERR_VALUE;
      seg->custom3 = val;
      stateChanged = true;
      break;
    case DT_SEG_CCT:
      if (val > 255) return PCCC_ERR_VALUE;
      seg->setCCT(val);
      break;
    case DT_SEG_REVERSE:
      if (val > 1) return PCCC_ERR_VALUE;
      seg->reverse = val;
      stateChanged = true;
      break;
    case DT_SEG_MIRROR:
      if (val > 1) return PCCC_ERR_VALUE;
      seg->mirror = val;
      stateChanged = true;
      break;
    default: return 0; // reserved elements accept and ignore writes
  }
  changed = true;
  return 0;
}


static EipPcccUsermod eip_pccc_usermod;
REGISTER_USERMOD(eip_pccc_usermod);
