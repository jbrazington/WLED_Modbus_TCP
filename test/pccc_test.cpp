// Host-side tests for the EtherNet/IP PCCC usermod: encapsulation, CIP (unconnected,
// Unconnected Send, Forward Open + connected messaging) and PCCC typed reads/writes.
#define private public
#include "../usermods/EIP_PCCC/EIP_PCCC.cpp"
#undef private
#include <cassert>
#include <arpa/inet.h>
bool stateChanged=false; byte briLast=128, bri=128, currentPreset=0, nightlightDelayMins=60, nightlightTargetBri=0, realtimeMode=0;
uint16_t transitionDelay=750; bool nightlightActive=false; int16_t currentPlaylist=-1; int stateUpdates=0, lastPreset=0; Strip strip;
char serverDescription[33]="WLED";
static EipPcccUsermod &um = eip_pccc_usermod;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } }while(0)
typedef std::vector<uint8_t> Bytes;
static Bytes cat(Bytes a, const Bytes& b){ a.insert(a.end(),b.begin(),b.end()); return a; }
static Bytes le16b(uint16_t v){ return {uint8_t(v),uint8_t(v>>8)}; }
static Bytes le32b(uint32_t v){ return {uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)}; }
static uint32_t session=0;

// encapsulation frame
static Bytes encap(uint16_t cmd, uint32_t sess, const Bytes& data){
  Bytes f=cat(le16b(cmd),le16b(data.size())); f=cat(f,le32b(sess)); f=cat(f,le32b(0));
  f=cat(f,{'c','o','n','t','e','x','t','!'}); f=cat(f,le32b(0)); return cat(f,data); }
static Bytes call(const Bytes& req){
  uint8_t out[EIP_FRAME_SIZE]; uint16_t n=um.processEncap(0,req.data(),req.size(),out);
  if(n==0||n==EIP_CLOSE) return {};
  assert(n>=24 && le16(out+2)==n-24 && memcmp(out+12,"context!",8)==0);
  return Bytes(out,out+n); }
static uint32_t status(const Bytes& r){ return le32(r.data()+8); }

// SendRRData with one CIP request, returns the CIP reply
static Bytes rr(const Bytes& cip){
  Bytes d=cat(le32b(0),le16b(0)); d=cat(d,le16b(2)); d=cat(d,{0,0,0,0, 0xB2,0}); d=cat(d,le16b(cip.size())); d=cat(d,cip);
  Bytes r=call(encap(EIP_CMD_SEND_RR_DATA,session,d));
  if(r.size()<40 || status(r)) return {};
  assert(le16(&r[30])==2 && le16(&r[32])==0 && le16(&r[36])==0xB2 && le16(&r[38])==r.size()-40);
  return Bytes(r.begin()+40,r.end()); }

static const Bytes REQ_ID={7, 0x4D,0x00, 0x11,0x22,0x33,0x44};
static Bytes executePccc(const Bytes& p){ return cat(cat({0x4B,2,0x20,0x67,0x24,0x01},REQ_ID),p); }
static uint16_t tns=0x100;
// PCCC request through SendRRData, returns the PCCC reply after checking the CIP header and TNS
static Bytes pccc(Bytes p){
  tns++; p[2]=uint8_t(tns); p[3]=uint8_t(tns>>8);
  Bytes c=rr(executePccc(p)); assert(c.size()>=15);
  assert(c[0]==0xCB && c[2]==0 && Bytes(c.begin()+4,c.begin()+11)==REQ_ID);
  Bytes r(c.begin()+11,c.end()); assert(r[0]==(p[0]|0x40) && le16(&r[2])==tns); return r; }
static uint16_t err(const Bytes& r){ return r[1]==0xF0 ? 0xF000|r[4] : r[1]<<8; }  // 0 = ok

static Bytes slcRead(uint8_t file, uint8_t type, uint8_t elem, uint8_t bytes){ return {0x0F,0,0,0,0xA2,bytes,file,type,elem,0}; }
static Bytes slcWrite(uint8_t file, uint8_t type, uint8_t elem, std::vector<uint16_t> w){
  Bytes p={0x0F,0,0,0,0xAA,uint8_t(w.size()*2),file,type,elem,0}; for(auto v:w) p=cat(p,le16b(v)); return p; }
static uint16_t word(const Bytes& r, int i){ return le16(&r[4+i*2]); }
static uint16_t rdN(uint8_t file, uint8_t elem){ Bytes r=pccc(slcRead(file,0x89,elem,2)); assert(!err(r)&&r.size()==6); return word(r,0); }
static uint16_t wrN(uint8_t file, uint8_t elem, uint16_t v){ return err(pccc(slcWrite(file,0x89,elem,{v}))); }

int main(){
  // ---- encapsulation ----
  Bytes r=call(encap(EIP_CMD_REGISTER_SESSION,0,{1,0,0,0}));
  CHECK(status(r)==0 && r.size()==28); session=le32(&r[4]); CHECK(session!=0 && um.clients[0].session==session);
  r=call(encap(EIP_CMD_REGISTER_SESSION,0,{2,0,0,0})); CHECK(status(r)==EIP_ERR_PROTOCOL && le16(&r[24])==1);
  r=call(encap(EIP_CMD_SEND_RR_DATA,session+1,{0,0,0,0,0,0,0,0})); CHECK(status(r)==EIP_ERR_SESSION);
  r=call(encap(EIP_CMD_SEND_RR_DATA,session,{0,0,0,0,0,0,1,0,0xB2,0,9,0})); CHECK(status(r)==EIP_ERR_DATA); // truncated item
  r=call(encap(0x0099,session,{})); CHECK(status(r)==EIP_ERR_COMMAND && r.size()==24);
  CHECK(call(encap(EIP_CMD_NOP,session,{1,2})).empty());
  r=call(encap(EIP_CMD_LIST_SERVICES,0,{})); CHECK(status(r)==0 && le16(&r[26])==0x100 && le16(&r[32])==0x20 && !memcmp(&r[34],"Communications",14));
  r=call(encap(EIP_CMD_LIST_INTERFACES,0,{})); CHECK(status(r)==0 && r.size()==26 && le16(&r[24])==0);
  um.setup(); um.serverPort=44818;
  r=call(encap(EIP_CMD_LIST_IDENTITY,0,{}));
  { const uint8_t *it=&r[30]; CHECK(le16(&r[24])==1 && le16(&r[26])==0x0C && le16(&r[28])==r.size()-30);
    CHECK(le16(it)==1 && it[3]==2 && it[4]==0xAF && it[5]==0x12 && it[6]==127 && it[9]==1);  // AF_INET, port 44818 BE, 127.0.0.1
    CHECK(le16(it+18)==EIP_PCCC_VENDOR_ID && le32(it+28)==0xC4123456 && it[32]==4 && !memcmp(it+33,"WLED",4) && it[37]==3); }

  // ---- CIP objects ----
  Bytes c=rr({0x01,2,0x20,0x01,0x24,0x01}); CHECK(c.size()==4+19 && c[0]==0x81 && c[2]==0 && le32(&c[14])==0xC4123456 && c[18]==4);
  c=rr({0x0E,3,0x20,0x01,0x24,0x01,0x30,0x07}); CHECK(c[0]==0x8E && c[2]==0 && c.size()==9 && c[4]==4);
  c=rr({0x0E,3,0x20,0x01,0x24,0x01,0x30,0x09}); CHECK(c[0]==0x8E && c[2]==CIP_ERR_ATTRIBUTE);
  c=rr({0x4C,2,0x20,0x6B,0x24,0x01}); CHECK(c[0]==0xCC && c[2]==CIP_ERR_PATH_DEST);
  c=rr({0x4C,2,0x20,0x67,0x24,0x01}); CHECK(c[0]==0xCC && c[2]==CIP_ERR_SERVICE);
  c=rr({0x4B,1,0x91,0x02}); CHECK(c[2]==CIP_ERR_PATH_SEGMENT);
  c=rr({0x4B,3,0x21,0x00,0x67,0x00,0x24,0x01}); CHECK(c[0]==0xCB && c[2]==CIP_ERR_NOT_ENOUGH_DATA); // 16 bit class, no PCCC data
  c=rr({0x4B,2,0x20,0x67,0x24,0x01,7,0x4D,0,1,2,3,4,0x0F,0,1,0,0x2F}); CHECK(c[0]==0xCB && c[2]==0 && c[11]==0x4F && c[12]==0x10); // unknown FNC

  // ---- SLC typed read / write (MicroLogix, SLC 5/05, Logix "SLC Typed Read/Write") ----
  r=pccc(slcRead(7,0x89,0,6)); CHECK(!err(r) && r.size()==10 && word(r,0)==1 && word(r,1)==128 && word(r,2)==0);
  r=pccc(slcRead(7,0x89,0,32)); CHECK(!err(r) && r.size()==36 && word(r,3)==750);  // whole file
  CHECK(wrN(7,1,200)==0 && bri==200 && rdN(7,1)==200);
  CHECK(wrN(7,1,300)==PCCC_ERR_VALUE);
  CHECK(err(pccc(slcRead(7,0x85,0,2)))==PCCC_ERR_ADDRESS);   // N7 read as a B file
  CHECK(err(pccc(slcRead(7,0x89,15,4)))==PCCC_ERR_SIZE);     // past the end of N7
  CHECK(err(pccc(slcRead(99,0x89,0,2)))==PCCC_ERR_ADDRESS);  // no such file
  CHECK(err(pccc(slcRead(7,0x89,0,3)))==PCCC_ERR_SIZE);      // odd byte count
  CHECK(err(pccc({0x0F,0,0,0,0xA2,2,7,0x89,0,1}))==PCCC_ERR_ADDRESS); // sub-element
  CHECK(err(pccc({0x0F,0,0,0,0xA2,2,7}))==PCCC_ERR_DECODE);  // truncated
  r=pccc({0x0F,0,0,0,0xA1,2,7,0x89,1}); CHECK(!err(r) && word(r,0)==200);   // 2 address fields
  r=pccc({0x0F,0,0,0,0xA2,2,0xFF,7,0,0x89,0xFF,1,0,0}); CHECK(!err(r) && word(r,0)==200); // 0xFF extended fields
  CHECK(wrN(8,0,1)==PCCC_ERR_ACCESS);                        // status file is read only
  r=pccc(slcRead(8,0x89,0,28)); CHECK(!err(r) && word(r,0)==60 && word(r,1)==1 && word(r,2)==187 && word(r,3)==72 && (int16_t)word(r,9)==-60);
  um.readOnly=true; CHECK(wrN(7,1,100)==PCCC_ERR_PROTECT && bri==200); um.readOnly=false;
  CHECK(err(pccc(slcWrite(7,0x89,15,{0,0})))==PCCC_ERR_SIZE && bri==200); // range checked before anything is written
  // block write: power off + brightness 150 stays off, brightness kept for next power on
  CHECK(err(pccc(slcWrite(7,0x89,0,{0,150})))==0 && bri==0 && rdN(7,1)==150);
  CHECK(wrN(7,0,1)==0 && bri==150);
  CHECK(wrN(7,0,2)==PCCC_ERR_VALUE);
  // transition, nightlight, main segment
  CHECK(err(pccc(slcWrite(7,0x89,3,{1000,30,100})))==0 && transitionDelay==1000 && nightlightDelayMins==30 && nightlightTargetBri==100);
  CHECK(wrN(7,4,0)==PCCC_ERR_VALUE && wrN(7,6,1)==PCCC_ERR_VALUE && wrN(7,6,0)==0);
  // preset is edge triggered
  lastPreset=0; CHECK(wrN(7,2,3)==0 && lastPreset==3);
  lastPreset=0; CHECK(wrN(7,2,3)==0 && lastPreset==0);
  CHECK(wrN(7,2,0)==0 && wrN(7,2,3)==0 && lastPreset==3); CHECK(wrN(7,2,251)==PCCC_ERR_VALUE);

  // ---- segments: N10 = segment 0 ----
  CHECK(wrN(10,2,9)==0 && strip.segs[0].mode==9); CHECK(wrN(10,2,187)==PCCC_ERR_VALUE);
  CHECK(err(pccc(slcWrite(10,0x89,10,{255,10,20,128})))==0 && strip.segs[0].colors[0]==RGBW32(255,10,20,128));
  r=pccc(slcRead(10,0x89,0,50)); CHECK(!err(r) && word(r,0)==1 && word(r,2)==9 && word(r,10)==255 && word(r,13)==128 && word(r,22)==127);
  CHECK(wrN(10,3,40)==0 && strip.segs[0].speed==40); CHECK(wrN(10,5,72)==PCCC_ERR_VALUE); CHECK(wrN(10,8,32)==PCCC_ERR_VALUE);
  CHECK(wrN(10,1,0)==0 && !strip.segs[0].on); CHECK(wrN(10,1,90)==0 && !strip.segs[0].on && strip.segs[0].opacity==90); CHECK(wrN(10,0,1)==0 && strip.segs[0].on);
  CHECK(wrN(10,23,1)==0 && strip.segs[0].reverse); CHECK(wrN(10,23,0)==0);
  CHECK(rdN(11,2)==0); CHECK(wrN(11,2,1)==PCCC_ERR_ADDRESS);  // segment 1 doesn't exist
  CHECK(err(pccc(slcRead(41,0x89,0,2)))==0 && err(pccc(slcRead(42,0x89,0,2)))==PCCC_ERR_ADDRESS); // N41 = segment 31 is last

  // ---- PLC-5 typed read / write (Logix "PLC5 Typed Read/Write", PLC-5) ----
  r=pccc({0x0F,0,0,0,0x68, 0,0, 3,0, 0x06,7,0, 3,0});
  CHECK(!err(r) && r.size()==4+3+6 && r[4]==0x97 && r[5]==0x09 && r[6]==0x42 && le16(&r[7])==1 && le16(&r[9])==150);
  r=pccc({0x0F,0,0,0,0x68, 0,0, 10,0, 0x06,10,0, 10,0});
  CHECK(!err(r) && r.size()==4+4+20 && r[4]==0x99 && r[5]==0x09 && r[6]==21 && r[7]==0x42 && le16(&r[8+2*2])==9);
  r=pccc({0x0F,0,0,0,0x68, 2,0, 1,0, 0x06,7,0, 1,0}); CHECK(!err(r) && le16(&r[7])==0);           // packet offset: N7:2
  r=pccc({0x0F,0,0,0,0x68, 0,0, 1,0, 0x0E,7,1,0, 1,0}); CHECK(!err(r) && le16(&r[7])==150);     // with sub-element level 0
  r=pccc({0x0F,0,0,0,0x68, 0,0, 1,0, 0x07,0,7,1, 1,0}); CHECK(!err(r) && le16(&r[7])==150);     // with data table level
  r=pccc({0x0F,0,0,0,0x68, 0,0, 1,0, 0x06,0xFF,7,0,0xFF,1,0, 1,0}); CHECK(!err(r) && le16(&r[7])==150);
  r=pccc({0x0F,0,0,0,0x68, 0,0, 1,0, 0x06,3,0, 1,0}); CHECK(!err(r) && r[6]==0x22);            // B3 is a bit string
  r=pccc({0x0F,0,0,0,0x68, 0,0, 2,0, 0,'$','N','7',':','0',0, 2,0}); CHECK(!err(r) && le16(&r[7])==1 && le16(&r[9])==150); // ASCII address
  CHECK(err(pccc({0x0F,0,0,0,0x68, 0,0, 1,0, 0,'$','N','7',':','0','/','1',0, 1,0}))==PCCC_ERR_ADDRESS);
  CHECK(err(pccc({0x0F,0,0,0,0x68, 0,0, 1,0, 0x0E,7,0,1, 1,0}))==PCCC_ERR_ADDRESS);          // sub-element 1
  CHECK(err(pccc({0x0F,0,0,0,0x68, 0,0, 117,0, 0x06,7,0, 117,0}))==PCCC_ERR_SIZE);
  CHECK(err(pccc({0x0F,0,0,0,0x68, 0,0, 1,0, 0x06,7,16, 1,0}))==PCCC_ERR_SIZE);
  // typed write: array of 2 INT to N10:3 (speed, intensity), then a single INT
  r=pccc({0x0F,0,0,0,0x67, 0,0, 2,0, 0x06,10,3, 0x95,0x09,0x42, 77,0, 88,0}); CHECK(!err(r) && r.size()==4 && strip.segs[0].speed==77 && strip.segs[0].intensity==88);
  r=pccc({0x0F,0,0,0,0x67, 0,0, 1,0, 0x06,7,1, 0x42, 99,0}); CHECK(!err(r) && bri==99);
  r=pccc({0x0F,0,0,0,0x67, 0,0, 1,0, 0x06,7,1, 0x42, 98,0, 0}); CHECK(!err(r) && bri==98);       // padded
  CHECK(err(pccc({0x0F,0,0,0,0x67, 0,0, 1,0, 0x06,7,1, 0x94,0x08, 0,0,0,0}))==PCCC_ERR_DECODE); // REAL not supported
  // word range read / write
  r=pccc({0x0F,0,0,0,0x01, 0,0, 2,0, 0x06,7,0, 4}); CHECK(!err(r) && r.size()==8 && word(r,1)==98);
  r=pccc({0x0F,0,0,0,0x00, 0,0, 1,0, 0x06,7,1, 120,0}); CHECK(!err(r) && bri==120);
  // read-modify-write on B3:0 (power off), then back on via the N7 file
  r=pccc({0x0F,0,0,0,0x26, 0x06,3,0, 0xFE,0xFF, 0,0}); CHECK(!err(r) && bri==0);
  CHECK(err(pccc({0x0F,0,0,0,0x26, 0x06,3,0, 0xFF,0xFF, 1,0, 0x06,99,0, 0xFF,0xFF,0,0}))==PCCC_ERR_ADDRESS && bri==0); // atomic
  CHECK(wrN(7,0,1)==0 && bri==120);

  // ---- B3 bits, SLC masked write (bit writes from MicroLogix/SLC/Logix) ----
  r=pccc(slcRead(3,0x85,0,6)); CHECK(!err(r) && word(r,0)==DT_BIT_POWER && word(r,1)==1 && word(r,2)==0);
  r=pccc({0x0F,0,0,0,0xAB,2,3,0x85,0,0, 0x01,0x00, 0x00,0x00}); CHECK(!err(r) && bri==0);        // B3:0/0 = 0
  r=pccc({0x0F,0,0,0,0xAB,2,3,0x85,0,0, 0x01,0x00, 0x01,0x00}); CHECK(!err(r) && bri==120);      // B3:0/0 = 1
  r=pccc({0x0F,0,0,0,0xAB,2,3,0x85,0,0, 0x02,0x00, 0xFF,0xFF}); CHECK(!err(r) && nightlightActive && bri==120); // only the masked bit
  r=pccc({0x0F,0,0,0,0xAB,2,3,0x85,1,0, 0x01,0x00, 0x00,0x00}); CHECK(!err(r) && !strip.segs[0].on);
  CHECK(err(pccc(slcWrite(3,0x85,1,{0xFFFF})))==0 && strip.segs[0].on);  // missing segments ignored
  CHECK(err(pccc(slcWrite(3,0x85,0,{DT_BIT_POWER|DT_BIT_LIVE})))==0 && !nightlightActive && realtimeMode==0); // status bits ignored
  CHECK(err(pccc({0x0F,0,0,0,0xAB,2,7,0x89,1,0, 0xF0,0x00, 0x30,0x00}))==0 && bri==0x38);  // masked write on N7:1: 120 = 0x78 -> 0x38
  CHECK(err(pccc({0x0F,0,0,0,0xAB,2,8,0x89,0,0, 1,0, 1,0}))==PCCC_ERR_ACCESS);

  // echo and unsupported commands
  r=pccc({0x06,0,0,0,0x00, 1,2,3}); CHECK(r[0]==0x46 && r[1]==0 && r.size()==7 && r[4]==1 && r[6]==3);
  r=pccc({0x01,0,0,0, 0,0}); CHECK(r[0]==0x41 && r[1]==0x10);

  // ---- Unconnected Send (Logix MSG routed through the backplane/ENBT) ----
  { Bytes emb=executePccc({0x0F,0,0x34,0x12,0xA2,2,7,0x89,1,0});
    Bytes ucs=cat({0x52,2,0x20,0x06,0x24,0x01, 0x0A,0x05}, le16b(emb.size())); ucs=cat(ucs,emb); if(emb.size()&1) ucs.push_back(0);
    ucs=cat(ucs,{1,0,1,0});
    c=rr(ucs); CHECK(c.size()==4+7+6 && c[0]==0xCB && c[11]==0x4F && c[12]==0 && le16(&c[13])==0x1234 && le16(&c[15])==0x38); }

  // ---- Forward Open, connected messaging, Forward Close ----
  Bytes fo={0x54,2,0x20,0x06,0x24,0x01, 0x0A,0x0E}; fo=cat(fo,le32b(0)); fo=cat(fo,le32b(0xC0FFEE01));
  fo=cat(fo,{0x34,0x12, 0x4D,0x00, 0x11,0x22,0x33,0x44, 1,0,0,0}); fo=cat(fo,le32b(2000000)); fo=cat(fo,le16b(0x43F4));
  fo=cat(fo,le32b(2000000)); fo=cat(fo,le16b(0x43F4)); fo=cat(fo,{0xA3, 2, 0x20,0x02,0x24,0x01});
  c=rr(fo); CHECK(c.size()==30 && c[0]==0xD4 && c[2]==0 && le32(&c[8])==0xC0FFEE01 && le16(&c[12])==0x1234 && le32(&c[20])==2000000);
  uint32_t ot=le32(&c[4]); CHECK(ot!=0 && um.conns[0].used && um.conns[0].timeoutMs==16000);
  auto unit=[&](uint32_t id, uint16_t seq, const Bytes& cip){
    Bytes d=cat(le32b(0),le16b(0)); d=cat(d,le16b(2)); d=cat(d,{0xA1,0,4,0}); d=cat(d,le32b(id)); d=cat(d,{0xB1,0});
    d=cat(d,le16b(cip.size()+2)); d=cat(d,le16b(seq)); d=cat(d,cip); return call(encap(EIP_CMD_SEND_UNIT_DATA,session,d)); };
  r=unit(ot,7,executePccc({0x0F,0,0x99,0,0xA2,2,7,0x89,1,0}));
  CHECK(r.size()==24+22+17 && le16(&r[0])==EIP_CMD_SEND_UNIT_DATA && status(r)==0 && le32(&r[36])==0xC0FFEE01 && le16(&r[40])==0xB1 && le16(&r[42])==19 && le16(&r[44])==7);
  CHECK(r[46]==0xCB && r[57]==0x4F && r[58]==0 && le16(&r[61])==0x38);
  CHECK(unit(ot+1,8,executePccc({0x0F,0,0x99,0,0xA2,2,7,0x89,1,0})).empty());  // unknown connection: silent
  c=rr(fo); CHECK(c[2]==0 && um.conns[1].used==false);                           // same triad reopens the same slot
  ot=le32(&c[4]);
  Bytes lfo={0x5B,2,0x20,0x06,0x24,0x01, 0x0A,0x0E}; lfo=cat(lfo,le32b(0)); lfo=cat(lfo,le32b(0xC0FFEE02));
  lfo=cat(lfo,{0x35,0x12, 0x4D,0x00, 0x11,0x22,0x33,0x44, 1,0,0,0}); lfo=cat(lfo,le32b(500000)); lfo=cat(lfo,le32b(0x42000FA0));
  lfo=cat(lfo,le32b(500000)); lfo=cat(lfo,le32b(0x42000FA0)); lfo=cat(lfo,{0xA3, 2, 0x20,0x02,0x24,0x01});
  c=rr(lfo); CHECK(c.size()==30 && c[0]==0xDB && c[2]==0 && um.conns[1].used && um.conns[1].timeoutMs==4000);
  Bytes io=fo; io[6+34]=0x01; io[6+10]=0x99; c=rr(io); CHECK(c[0]==0xD4 && c[2]==CIP_ERR_CONNECTION && c[3]==1 && le16(&c[4])==CIP_EXT_TRANSPORT && c.size()==16);
  Bytes fc={0x4E,2,0x20,0x06,0x24,0x01, 0x0A,0x0E, 0x34,0x12, 0x4D,0x00, 0x11,0x22,0x33,0x44, 3,0, 0x20,0x02,0x24,0x01};
  c=rr(fc); CHECK(c.size()==14 && c[0]==0xCE && c[2]==0 && le16(&c[4])==0x1234 && !um.conns[0].used);
  c=rr(fc); CHECK(c[0]==0xCE && c[2]==CIP_ERR_CONNECTION && le16(&c[4])==CIP_EXT_NOT_FOUND);
  CHECK(unit(ot,9,executePccc({0x0F,0,0x99,0,0xA2,2,7,0x89,1,0})).empty());     // closed
  // too many connections
  for(int i=0;i<EIP_PCCC_MAX_CONNECTIONS+1;i++){ Bytes f=fo; f[6+10]=0x50+i; c=rr(f); CHECK((c[2]==0) == (i<EIP_PCCC_MAX_CONNECTIONS-1)); }
  CHECK(le16(&c[4])==CIP_EXT_NO_CONNECTIONS);
  // connection timeout
  um.conns[1].lastSeen=millis()-5000; um.enabled=true; um.server=new WiFiServer(0); um.loop(); CHECK(!um.conns[1].used);
  delete um.server; um.server=nullptr;
  // closing the TCP client closes its connections
  um.dropClient(0); for(int i=0;i<EIP_PCCC_MAX_CONNECTIONS;i++) CHECK(!um.conns[i].used); CHECK(um.clients[0].session==0);
  r=call(encap(EIP_CMD_REGISTER_SESSION,0,{1,0,0,0})); session=le32(&r[4]);
  CHECK(call(encap(EIP_CMD_UNREGISTER,session,{})).empty());

  // ---- cyclic writes don't change state ----
  { r=call(encap(EIP_CMD_REGISTER_SESSION,0,{1,0,0,0})); session=le32(&r[4]);
    um.changed=false; stateChanged=false; lastPreset=0;
    for(int k=0;k<5;k++){ pccc(slcWrite(7,0x89,0,{1,0x38,3})); pccc(slcWrite(10,0x89,0,{1,90,9,77,88})); }
    CHECK(!um.changed && !stateChanged && lastPreset==0); }

  // ---- over real sockets through loop(): TCP with split frames, UDP List Identity ----
  { um.enabled=true; um.port=15818; um.discovery=true; um.stopServer(); um.startServer(); CHECK(um.server && um.udpActive);
    int s=socket(AF_INET,SOCK_STREAM,0); sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(15818); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(connect(s,(sockaddr*)&a,sizeof a)==0);
    auto xfer=[&](const Bytes& req, size_t split){ send(s,req.data(),split,0); um.loop(); um.loop(); if(split<req.size()) send(s,req.data()+split,req.size()-split,0);
      Bytes resp(EIP_FRAME_SIZE); int n=-1; for(int i=0;i<200 && n<=0;i++){ um.loop(); n=recv(s,resp.data(),resp.size(),MSG_DONTWAIT); usleep(1000); }
      resp.resize(n>0?n:0); return resp; };
    r=xfer(encap(EIP_CMD_REGISTER_SESSION,0,{1,0,0,0}),10); CHECK(r.size()==28 && status(r)==0); uint32_t sess=le32(&r[4]);
    Bytes cip=executePccc({0x0F,0,0x42,0,0x68, 0,0, 2,0, 0x06,7,0, 2,0});
    Bytes d=cat(le32b(0),le16b(0)); d=cat(d,le16b(2)); d=cat(d,{0,0,0,0, 0xB2,0}); d=cat(d,le16b(cip.size())); d=cat(d,cip);
    r=xfer(encap(EIP_CMD_SEND_RR_DATA,sess,d),30); CHECK(r.size()==24+16+11+4+7 && r[40]==0xCB && r[51]==0x4F && r[55]==0x95 && le16(&r[58])==1);
    // two frames in one write
    Bytes two=cat(encap(EIP_CMD_LIST_INTERFACES,0,{}),encap(EIP_CMD_LIST_INTERFACES,0,{}));
    r=xfer(two,two.size()); for(int i=0;i<50 && r.size()<52;i++){ um.loop(); uint8_t b[64]; int n=recv(s,b,64,MSG_DONTWAIT); if(n>0) r.insert(r.end(),b,b+n); usleep(1000); }
    CHECK(r.size()==52);
    close(s);
    int u=socket(AF_INET,SOCK_DGRAM,0); Bytes li=encap(EIP_CMD_LIST_IDENTITY,0,{}); sendto(u,li.data(),li.size(),0,(sockaddr*)&a,sizeof a);
    uint8_t b[200]; int n=-1; for(int i=0;i<200 && n<=0;i++){ um.loop(); n=recv(u,b,sizeof b,MSG_DONTWAIT); usleep(1000); }
    CHECK(n>30 && le16(b)==EIP_CMD_LIST_IDENTITY && le16(b+26)==0x0C && b[34]==0x3D && b[35]==0xCA); // port 15818
    close(u); um.stopServer(); }

  // config round trip
  { DynamicJsonDocument doc(1024); JsonObject root=doc.to<JsonObject>(); um.addToConfig(root);
    root["EIP_PCCC"]["segmentFile"]=20; root["EIP_PCCC"]["controlFile"]=1000; CHECK(um.readFromConfig(root));
    CHECK(um.segmentFile==20 && um.controlFile==DT_CONTROL_FILE); }

  // malformed input must never read or write out of bounds (run under -fsanitize=address to check)
  { r=call(encap(EIP_CMD_REGISTER_SESSION,0,{1,0,0,0})); session=le32(&r[4]); srand(1);
    Bytes seeds[]={executePccc({0x0F,0,0,0,0x68,0,0,2,0,0x06,7,0,2,0}), executePccc({0x0F,0,0,0,0xAB,2,3,0x85,0,0,1,0,1,0}),
      executePccc({0x0F,0,0,0,0x26,0x06,3,0,0xFE,0xFF,0,0}), executePccc({0x0F,0,0,0,0x67,0,0,1,0,0,'$','N','7',':','1',0,0x42,9,0}), fo, fc};
    uint8_t out[EIP_FRAME_SIZE]; uint32_t fuzzConn=0;
    for(int k=0;k<200000;k++){ Bytes cip=seeds[k%6]; int m=1+rand()%4;
      if(k%1000==0){ Bytes f=fo; f[6+10]=0x77; c=rr(f); if(c[2]==0) fuzzConn=le32(&c[4]); }
      for(int j=0;j<m;j++){ int op=rand()%3, pos=rand()%(cip.size()+1);
        if(op==0 && pos<(int)cip.size()) cip[pos]=rand(); else if(op==1) cip.insert(cip.begin()+pos,uint8_t(rand())); else if(pos<(int)cip.size()) cip.resize(pos); }
      Bytes d=cat(le32b(0),le16b(0)); d=cat(d,le16b(2));
      if(k&1){ d=cat(d,{0,0,0,0, 0xB2,0}); d=cat(d,le16b(cip.size())); }
      else { d=cat(d,{0xA1,0,4,0}); d=cat(d,le32b(fuzzConn)); d=cat(d,{0xB1,0}); d=cat(d,le16b(cip.size()+2)); d=cat(d,{1,0}); }
      d=cat(d,cip); if(rand()%8==0) d.resize(rand()%(d.size()+1));
      Bytes f=encap(k&1?EIP_CMD_SEND_RR_DATA:EIP_CMD_SEND_UNIT_DATA,session,d);
      uint16_t n=um.processEncap(0,f.data(),f.size(),out); CHECK(n==0 || (n>=24 && n<=EIP_FRAME_SIZE)); }
  }

  printf(fails? "%d FAILURES\n":"ALL PASS\n", fails); return fails; }
