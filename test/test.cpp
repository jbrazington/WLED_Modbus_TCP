#define private public
#include "../usermods/Modbus_TCP/Modbus_TCP.cpp"
#undef private
#include <cassert>
bool stateChanged=false; byte briLast=128, bri=128, currentPreset=0, nightlightDelayMins=60, nightlightTargetBri=0, realtimeMode=0;
uint16_t transitionDelay=750; bool nightlightActive=false; int16_t currentPlaylist=-1; int stateUpdates=0, lastPreset=0; Strip strip;
static ModbusTCPUsermod &um = modbus_tcp_usermod;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } }while(0)
// send PDU, return response PDU in out, length
static int req(std::vector<uint8_t> pdu, std::vector<uint8_t>& out, uint8_t unit=1){
  uint8_t f[260]; f[0]=0x12; f[1]=0x34; f[2]=0; f[3]=0; f[4]=0; f[5]=pdu.size()+1; f[6]=unit; memcpy(f+7,pdu.data(),pdu.size());
  int n=um.processFrame(f,7+pdu.size()); if(!n) return 0;
  assert(f[0]==0x12&&f[1]==0x34&&f[6]==unit); assert(((f[4]<<8)|f[5])==n-6);
  out.assign(f+7,f+n); return out.size(); }
static uint16_t rd(uint16_t a, uint8_t fc=3){ std::vector<uint8_t> o; req({fc,uint8_t(a>>8),uint8_t(a),0,1},o); assert(o[0]==fc); return (o[2]<<8)|o[3]; }
static uint8_t wr(uint16_t a, uint16_t v){ std::vector<uint8_t> o; req({6,uint8_t(a>>8),uint8_t(a),uint8_t(v>>8),uint8_t(v)},o); return o[0]&0x80 ? o[1] : 0; }
int main(){
  std::vector<uint8_t> o;
  // read globals
  CHECK(rd(1)==128); CHECK(rd(0)==1); CHECK(rd(3)==750);
  // brightness off/on
  CHECK(wr(1,0)==0); CHECK(bri==0); CHECK(rd(0)==0);
  CHECK(wr(0,1)==0); CHECK(bri==128);
  CHECK(wr(1,200)==0); CHECK(bri==200);
  CHECK(wr(1,256)==3);
  // power coil
  req({5,0,0,0x00,0x00},o); CHECK(o.size()==5 && o[0]==5); CHECK(bri==0);
  req({1,0,0,0,2},o); CHECK(o[0]==1 && o[1]==1 && o[2]==0);
  req({5,0,0,0xFF,0x00},o); CHECK(bri==200);
  req({1,0,0,0,2},o); CHECK(o[2]==1);
  req({5,0,0,0x12,0x00},o); CHECK(o[0]==0x85 && o[1]==3);
  // preset
  CHECK(wr(2,5)==0 && lastPreset==5); CHECK(wr(2,251)==3);
  // segment 0: effect, speed, colors via FC16
  CHECK(wr(102,9)==0 && strip.segs[0].mode==9); CHECK(wr(102,187)==3);
  req({0x10,0,110,0,4,8, 0,255, 0,10, 0,20, 0,128},o); CHECK(o.size()==5 && o[0]==0x10 && o[4]==4);
  CHECK(strip.segs[0].colors[0]==RGBW32(255,10,20,128));
  CHECK(rd(110)==255 && rd(113)==128);
  // block read of segment 0
  req({3,0,100,0,25},o); CHECK(o[1]==50 && ((o[2+2*2]<<8|o[3+2*2])==9));
  // seg 1 doesn't exist
  CHECK(rd(200)==0); CHECK(wr(200,1)==2);
  // out of range
  req({3,0x0C,0xE4,0,1},o); CHECK(o[0]==0x83 && o[1]==2); // 3300
  req({3,0x0C,0xE3,0,1},o); CHECK(o[0]==3); // 3299 ok
  req({3,0,0,0,126},o); CHECK(o[0]==0x83 && o[1]==3);
  // partial write rejected atomically: write 99..100? both valid; write 3298..3300 invalid
  req({0x10,0x0C,0xE2,0,3,6,0,1,0,1,0,1},o); CHECK(o[0]==0x90 && o[1]==2);
  // input regs
  CHECK(rd(0,4)==60); CHECK(rd(1,4)==1); CHECK(rd(2,4)==187); CHECK((int16_t)rd(9,4)==-60); CHECK(rd(102,4)==60);
  // discrete
  req({2,0,0,0,5},o); CHECK(o[0]==2 && o[1]==1 && (o[2]&1) && (o[2]&0x10));
  req({2,0,100,0,2},o); CHECK(o[2]==1);
  // write multiple coils: seg0 on off via coil 100
  req({0x0F,0,100,0,1,1,0},o); CHECK(o[0]==0x0F && !strip.segs[0].on);
  req({0x0F,0,100,0,1,1,1},o); CHECK(strip.segs[0].on);
  // unknown fc
  req({0x2B,0x0E,1,0},o); CHECK(o[0]==0xAB && o[1]==1);
  // unit filter
  um.unitId=5; CHECK(req({3,0,1,0,1},o,1)==0); CHECK(req({3,0,1,0,1},o,5)>0); um.unitId=0;
  // read only
  um.readOnly=true; CHECK(wr(1,10)==1); um.readOnly=false;
  // garbage-length frames
  req({3,0,1},o); CHECK(o[0]==0x83);
  req({0x10,0,1,0,2,4,0,1},o); CHECK(o[0]==0x90 && o[1]==3);
  um.enabled=true; um.server=new WiFiServer(15020); um.server->begin();
  // Jason bug: block write power=0 + brightness=200 must turn off and stay off
  um.loop(); bri=200; um.loop();
  for (int k=0;k<5;k++){ req({0x10,0,0,0,2,4, 0,0, 0,200},o); CHECK(o[0]==0x10); um.loop(); CHECK(bri==0); }
  { std::vector<uint8_t> r; req({3,0,0,0,2},r); CHECK(r[3]==0 && r[5]==200); }
  // brightness while off only stores the level
  CHECK(wr(1,50)==0); CHECK(bri==0); CHECK(rd(1)==50);
  CHECK(wr(0,1)==0); CHECK(bri==50);
  // repeated identical writes don't change state
  um.loop(); { int b=stateUpdates; for(int k=0;k<5;k++){ wr(1,50); wr(103,128); wr(102,9); req({5,0,0,0xFF,0},o);} um.changed ? 0 : 0; um.loop(); CHECK(stateUpdates==b); CHECK(!stateChanged); }
  // preset not re-applied when already active
  // preset is edge triggered: repeated writes of the same value don't re-apply, even after currentPreset is cleared
  lastPreset=0; currentPreset=0; CHECK(wr(2,5)==0 && lastPreset==0);
  CHECK(wr(2,6)==0 && lastPreset==6); lastPreset=0; currentPreset=0; CHECK(wr(2,6)==0 && lastPreset==0);
  CHECK(wr(2,0)==0 && lastPreset==0); CHECK(wr(2,6)==0 && lastPreset==6);
  // Jason's sequence: client keeps writing power=0, bri=200, preset=2 every cycle
  { lastPreset=0; req({0x10,0,0,0,3,6, 0,0, 0,200, 0,2},o); CHECK(lastPreset==2); bri=200; currentPreset=0; // preset applied and turned on
    for(int k=0;k<5;k++){ lastPreset=0; req({0x10,0,0,0,3,6, 0,0, 0,200, 0,2},o); CHECK(o[0]==0x10); um.loop(); currentPreset=0; CHECK(bri==0); CHECK(lastPreset==0); } }
  // segment opacity: 0 = off, nonzero sets level only
  CHECK(wr(101,0)==0 && !strip.segs[0].on); CHECK(wr(101,100)==0 && !strip.segs[0].on && strip.segs[0].opacity==100); CHECK(wr(100,1)==0 && strip.segs[0].on);
  // loop flushes state
  int before=stateUpdates; um.changed=true; um.loop(); CHECK(stateUpdates==before+1);
  printf(fails? "%d FAILURES\n":"ALL PASS\n", fails); return fails; }
