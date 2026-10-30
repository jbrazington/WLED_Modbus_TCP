#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <string>
#include <cerrno>
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include "ArduinoJson-v6.h"
using std::min;
using std::max;
typedef uint8_t byte;
#define PROGMEM
#define F(x) x
#define FPSTR(x) x
#define PSTR(x) x
#define snprintf_P snprintf
#define DEBUG_PRINTF_P(...)
inline unsigned long millis(){ static unsigned long t=1000; return t+=1; }
inline void oappend(const char*){}
template<typename T, typename U> bool getJsonValue(const JsonVariant& e, T& v, U d){ if(e.isNull()){v=d;return false;} v=e.as<T>(); return true; }
#define R(c) (uint8_t)((c)>>16) 
#define G(c) (uint8_t)((c)>>8)
#define B(c) (uint8_t)(c)
#define W(c) (uint8_t)((c)>>24)
#define RGBW32(r,g,b,w) ((uint32_t(w)<<24)|(uint32_t(r)<<16)|(uint32_t(g)<<8)|uint32_t(b))
#define SEG_OPTION_ON 2
#define CALL_MODE_DIRECT_CHANGE 1
#define REALTIME_MODE_INACTIVE 0
#define VERSION 2510230
extern bool stateChanged; extern byte bri, currentPreset, nightlightDelayMins, nightlightTargetBri, realtimeMode;
extern uint16_t transitionDelay; extern bool nightlightActive; extern int16_t currentPlaylist;
extern int stateUpdates, lastPreset;
struct Segment { uint32_t colors[3]={0,0,0}; uint16_t start=0, stop=60; bool on=true, reverse=false, mirror=false; mutable bool freeze=false;
  uint8_t opacity=255, cct=127, mode=0, speed=128, intensity=128, palette=0, custom1=128, custom2=128, custom3=16, grouping=1, spacing=0;
  bool isActive() const { return stop>start; } uint16_t length() const { return stop-start; }
  Segment& setColor(uint8_t s, uint32_t c){ colors[s]=c; stateChanged=true; return *this; }
  Segment& setOption(uint8_t, bool v){ on=v; stateChanged=true; return *this; }
  Segment& setOpacity(uint8_t o){ opacity=o; stateChanged=true; return *this; }
  Segment& setMode(uint8_t m){ mode=m; stateChanged=true; return *this; }
  Segment& setPalette(uint8_t p){ palette=p; stateChanged=true; return *this; }
  Segment& setCCT(uint16_t c){ cct=c; return *this; } };
struct Strip { std::vector<Segment> segs{Segment()}; uint8_t mainSeg=0;
  uint8_t getSegmentsNum(){return segs.size();} Segment& getSegment(unsigned i){return segs[i<segs.size()?i:mainSeg];}
  uint8_t getMainSegmentId(){return mainSeg;} void setMainSegmentId(uint8_t i){mainSeg=i;}
  uint8_t getModeCount(){return 187;} uint16_t getLengthTotal(){return 60;} uint16_t getFps(){return 42;}
  void setTransition(uint16_t){} void restartRuntime(){} };
extern Strip strip;
inline uint8_t getPaletteCount(){ return 72; }
extern byte briLast; inline void toggleOnOff(){ if(bri==0) bri=briLast; else { briLast=bri; bri=0; } stateChanged=true; }
inline bool applyPreset(byte p, byte){ lastPreset=p; return true; }
inline void unloadPlaylist(){ currentPlaylist=-1; }
inline void stateUpdated(byte){ stateUpdates++; if(bri) briLast=bri; stateChanged=false; }
namespace BusManager { inline uint16_t currentMilliamps(){return 300;} inline uint16_t ablMilliampsMax(){return 2000;} }
struct { uint32_t getFreeHeap(){return 100*1024;} } ESP;
struct { int RSSI(){return -60;} void macAddress(uint8_t*m){ const uint8_t d[6]={0x24,0x0A,0xC4,0x12,0x34,0x56}; memcpy(m,d,6); } } WiFi;
extern char serverDescription[33];
#define WLED_CONNECTED true
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
struct WiFiClient { int fd=-1; WiFiClient(int f=-1):fd(f){}
  explicit operator bool() const {return fd>=0;}
  bool connected(){ if(fd<0) return false; char c; int r=recv(fd,&c,1,MSG_PEEK|MSG_DONTWAIT); return r>0 || (r<0 && (errno==EAGAIN||errno==EWOULDBLOCK)); }
  void stop(){ if(fd>=0) close(fd); fd=-1; }
  int available(){ int n=0; if(fd<0) return 0; ioctl(fd,FIONREAD,&n); return n; }
  int read(uint8_t*b,int n){ return recv(fd,b,n,MSG_DONTWAIT); }
  void write(const uint8_t*b,int n){ send(fd,b,n,0); }
  void setNoDelay(bool){} };
struct WiFiServer { int fd=-1; uint16_t p; WiFiServer(uint16_t port):p(port){}
  void begin(){ fd=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,4); sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(p); bind(fd,(sockaddr*)&a,sizeof a); listen(fd,8); fcntl(fd,F_SETFL,O_NONBLOCK); }
  void end(){ close(fd); } void setNoDelay(bool){}
  WiFiClient available(){ int c=accept(fd,nullptr,nullptr); return WiFiClient(c); } };
struct IPAddress { uint8_t b[4]; IPAddress(uint8_t a=127,uint8_t c=0,uint8_t d=0,uint8_t e=1):b{a,c,d,e}{} uint8_t operator[](int i) const { return b[i]; } };
struct { IPAddress localIP(){ return IPAddress(); } } Network;
struct WiFiUDP { int fd=-1; sockaddr_in from{}, to{}; uint8_t in[600], out[600]; int ilen=0, ipos=0, olen=0;
  bool begin(uint16_t p){ fd=socket(AF_INET,SOCK_DGRAM,0); int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,4); sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(p); fcntl(fd,F_SETFL,O_NONBLOCK); return bind(fd,(sockaddr*)&a,sizeof a)==0; }
  void stop(){ if(fd>=0) close(fd); fd=-1; }
  int parsePacket(){ socklen_t l=sizeof from; ilen=recvfrom(fd,in,sizeof in,0,(sockaddr*)&from,&l); ipos=0; return ilen>0?ilen:0; }
  int read(uint8_t*b,int n){ n=std::min(n,ilen-ipos); memcpy(b,in+ipos,n); ipos+=n; return n; }
  IPAddress remoteIP(){ uint8_t*a=(uint8_t*)&from.sin_addr; return IPAddress(a[0],a[1],a[2],a[3]); }
  uint16_t remotePort(){ return ntohs(from.sin_port); }
  int beginPacket(IPAddress ip, uint16_t port){ to=sockaddr_in{}; to.sin_family=AF_INET; to.sin_port=htons(port); memcpy(&to.sin_addr,ip.b,4); olen=0; return 1; }
  size_t write(const uint8_t*b,size_t n){ memcpy(out+olen,b,n); olen+=n; return n; }
  int endPacket(){ return sendto(fd,out,olen,0,(sockaddr*)&to,sizeof to)==olen; } };
class Usermod { public: virtual ~Usermod(){} virtual void setup()=0; virtual void loop()=0; virtual void connected(){} virtual void addToJsonInfo(JsonObject&){} virtual void addToConfig(JsonObject&){} virtual bool readFromConfig(JsonObject&){return true;} virtual void appendConfigData(){} };
#define REGISTER_USERMOD(x)
