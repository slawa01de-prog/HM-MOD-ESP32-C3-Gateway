#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// Archived v2 source. Complete HM-UART frame forwarding for HB-RF-ETH.
// See project history/changelog for context.

#define HM_RX_PIN 6
#define HM_TX_PIN 5
#define HM_RESET_PIN 7
#define HM_BAUD 115200
#define HB_PORT 3008
#define HTTP_PORT 80
#define MAX_HB_CLIENTS 4

#define T_CONNECT 0
#define T_DISCONNECT 1
#define T_KEEPALIVE 2
#define T_LED 3
#define T_RESET 4
#define T_STARTCONN 5
#define T_STOPCONN 6
#define T_FRAME 7

HardwareSerial HM(1);
WiFiUDP hbUdp;
WebServer web(HTTP_PORT);
Preferences prefs;

struct HbClient {
  bool connected=false;
  bool started=false;
  IPAddress ip;
  uint16_t port=0;
  uint8_t endpointId=1;
  uint8_t txCounter=0;
  uint32_t lastRxMs=0;
};
HbClient clients[MAX_HB_CLIENTS];

uint16_t crc16Bidcos(const uint8_t *data,size_t len){
  uint16_t crc=0xD77F;
  while(len--){
    crc^=((uint16_t)(*data++))<<8;
    for(int i=0;i<8;i++) crc=(crc&0x8000)?(uint16_t)((crc<<1)^0x8005):(uint16_t)(crc<<1);
  }
  return crc;
}

class HmWireParser {
public:
  static const size_t CAP=2048;
  enum State {NO_DATA,LEN_HI,LEN_LO,FRAME_DATA};
  uint8_t buf[CAP]; size_t pos=0; State state=NO_DATA;
  bool escaped=false; uint16_t frameLen=0, framePos=0;
  void reset(){pos=0;state=NO_DATA;escaped=false;frameLen=0;framePos=0;}
  bool push(uint8_t chr){
    if(chr==0xFD){pos=0;escaped=false;state=LEN_HI;frameLen=0;framePos=0;buf[pos++]=chr;return false;}
    if(state==NO_DATA)return false;
    if(pos>=CAP){reset();return false;}
    buf[pos++]=chr;
    if(chr==0xFC&&!escaped){escaped=true;return false;}
    uint8_t d=chr;
    if(escaped){d=chr|0x80;escaped=false;}
    switch(state){
      case LEN_HI: frameLen=((uint16_t)d)<<8; state=LEN_LO; break;
      case LEN_LO: frameLen|=d; frameLen+=2; framePos=0; if(frameLen>1900) reset(); else state=FRAME_DATA; break;
      case FRAME_DATA: if(++framePos>=frameLen){state=NO_DATA;return true;} break;
      default: break;
    }
    return false;
  }
};
HmWireParser hmParser;

void hmResetPulse(){
  pinMode(HM_RESET_PIN,OUTPUT);
  digitalWrite(HM_RESET_PIN,LOW); delay(50);
  digitalWrite(HM_RESET_PIN,HIGH); delay(50);
}

bool sameEndpoint(const HbClient &c,const IPAddress &ip,uint16_t port){return c.connected&&c.ip==ip&&c.port==port;}
HbClient* findClient(const IPAddress &ip,uint16_t port){for(int i=0;i<MAX_HB_CLIENTS;i++)if(sameEndpoint(clients[i],ip,port))return &clients[i];return nullptr;}
HbClient* findOrCreateClient(const IPAddress &ip,uint16_t port){
  HbClient *c=findClient(ip,port); if(c)return c;
  for(int i=0;i<MAX_HB_CLIENTS;i++) if(!clients[i].connected){clients[i]=HbClient();clients[i].ip=ip;clients[i].port=port;clients[i].lastRxMs=millis();return &clients[i];}
  return nullptr;
}

void hbSendTyped(HbClient &c,uint8_t type,const uint8_t *payload,size_t plen){
  uint8_t pkt[1500]; if(plen+4>sizeof(pkt))return;
  pkt[0]=type; pkt[1]=c.txCounter++;
  if(plen)memcpy(pkt+2,payload,plen);
  uint16_t crc=crc16Bidcos(pkt,plen+2);
  pkt[plen+2]=(uint8_t)(crc>>8); pkt[plen+3]=(uint8_t)crc;
  hbUdp.beginPacket(c.ip,c.port); hbUdp.write(pkt,plen+4); hbUdp.endPacket();
}

void handleHbPacket(const uint8_t *data,size_t len,const IPAddress &ip,uint16_t port){
  if(len<4)return;
  uint16_t calc=crc16Bidcos(data,len-2), got=((uint16_t)data[len-2]<<8)|data[len-1];
  if(calc!=got)return;
  uint8_t type=data[0], requestCounter=data[1];
  const uint8_t *payload=data+2; size_t plen=len-4;
  HbClient *c=(type==T_CONNECT)?findOrCreateClient(ip,port):findClient(ip,port);
  if(!c)return;
  c->lastRxMs=millis();
  switch(type){
    case T_CONNECT:
      c->connected=true; c->started=false;
      if(plen==1&&payload[0]==1){c->endpointId+=2;uint8_t reply[2]={1,requestCounter};hbSendTyped(*c,T_CONNECT,reply,2);}
      else if(plen==2&&payload[0]==2){uint8_t clientEp=payload[1];if(clientEp==0)c->endpointId+=2;else c->endpointId=clientEp;uint8_t reply[3]={2,requestCounter,c->endpointId};hbSendTyped(*c,T_CONNECT,reply,3);}
      break;
    case T_DISCONNECT: *c=HbClient(); break;
    case T_KEEPALIVE: break;
    case T_RESET: hmResetPulse(); hmParser.reset(); break;
    case T_STARTCONN: c->started=true; break;
    case T_STOPCONN: c->started=false; break;
    case T_FRAME: if(plen){HM.write(payload,plen);HM.flush();} break;
  }
}

void pollHbUdp(){
  int n=hbUdp.parsePacket(); if(n<=0)return;
  uint8_t pkt[1500]; if(n>(int)sizeof(pkt)){while(hbUdp.available())hbUdp.read();return;}
  int rd=hbUdp.read(pkt,n); if(rd>0)handleHbPacket(pkt,rd,hbUdp.remoteIP(),hbUdp.remotePort());
}

void pumpHmUart(){
  while(HM.available()){
    uint8_t b=(uint8_t)HM.read();
    if(hmParser.push(b)){
      for(int i=0;i<MAX_HB_CLIENTS;i++) if(clients[i].connected&&clients[i].started) hbSendTyped(clients[i],T_FRAME,hmParser.buf,hmParser.pos);
      hmParser.reset();
    }
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(HM_RESET_PIN,OUTPUT);
  digitalWrite(HM_RESET_PIN,HIGH);
  HM.begin(HM_BAUD,SERIAL_8N1,HM_RX_PIN,HM_TX_PIN);
  // Wi-Fi provisioning/web UI omitted here for brevity in the archived snapshot.
  // See v1 and current stable source for the complete project evolution.
}

void loop(){pollHbUdp();pumpHmUart();delay(1);}
