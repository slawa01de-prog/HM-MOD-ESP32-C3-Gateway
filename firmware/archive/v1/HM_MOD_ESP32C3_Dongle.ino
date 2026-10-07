#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

#define HM_RX_PIN       6
#define HM_TX_PIN       5
#define HM_RESET_PIN    7
#define HM_BAUD         115200
#define HB_PORT         3008
#define RAW_TCP_PORT    2329
#define HTTP_PORT       80
#define MAX_HB_CLIENTS  4
#define UART_CHUNK      256

#define T_CONNECT       0
#define T_DISCONNECT    1
#define T_KEEPALIVE     2
#define T_LED           3
#define T_RESET         4
#define T_STARTCONN     5
#define T_STOPCONN      6
#define T_FRAME         7

HardwareSerial HM(1);
WiFiUDP hbUdp;
WiFiServer rawServer(RAW_TCP_PORT);
WiFiClient rawClient;
WebServer web(HTTP_PORT);
Preferences prefs;

struct HbClient {
  bool connected = false;
  bool started = false;
  IPAddress ip;
  uint16_t port = 0;
  uint8_t endpointId = 1;
  uint8_t txCounter = 0;
  uint32_t lastRxMs = 0;
};

HbClient clients[MAX_HB_CLIENTS];
uint32_t statConnects=0, statDisconnects=0, statBadCrc=0, statUnknown=0;
uint32_t statRxFrames=0, statTxFrames=0, statReset=0, statKeepaliveTimeout=0;
String moduleTag="";
bool moduleReady=false;
String wifiSsid, wifiPass, hostName;

uint16_t crc16Bidcos(const uint8_t *data, size_t len) {
  uint16_t crc = 0xD77F;
  for (size_t i=0;i<len;i++) {
    crc ^= ((uint16_t)data[i]) << 8;
    for (int b=0;b<8;b++) crc = (crc & 0x8000) ? (uint16_t)((crc<<1)^0x8005) : (uint16_t)(crc<<1);
  }
  return crc;
}

void hmResetPulse() {
  pinMode(HM_RESET_PIN, OUTPUT);
  digitalWrite(HM_RESET_PIN, LOW); delay(50);
  digitalWrite(HM_RESET_PIN, HIGH); delay(50);
  pinMode(HM_RESET_PIN, INPUT_PULLUP);
  statReset++;
}

void hmSendEscaped(uint8_t b) {
  if (b==0xFC || b==0xFD) { HM.write(0xFC); HM.write(b ^ 0x80); }
  else HM.write(b);
}

void hmSendFrame(uint8_t dst, uint8_t cnt, const uint8_t *payload, size_t plen) {
  uint16_t packetLen = 2 + plen;
  uint8_t raw[512];
  if (5 + plen > sizeof(raw)) return;
  raw[0]=0xFD; raw[1]=(packetLen>>8)&0xFF; raw[2]=packetLen&0xFF; raw[3]=dst; raw[4]=cnt;
  if (plen) memcpy(raw+5,payload,plen);
  uint16_t crc=crc16Bidcos(raw,5+plen);
  HM.write(0xFD);
  for (size_t i=1;i<5+plen;i++) hmSendEscaped(raw[i]);
  hmSendEscaped((crc>>8)&0xFF); hmSendEscaped(crc&0xFF); HM.flush();
}

size_t hmReadEncodedFrame(uint8_t *out, size_t cap, uint32_t timeoutMs) {
  uint32_t start=millis(); size_t n=0; bool inFrame=false, esc=false;
  uint16_t decodedNeed=0, decodedSeen=0, packetLen=0; uint8_t lenHi=0;
  while (millis()-start < timeoutMs) {
    while (HM.available()) {
      uint8_t b=HM.read();
      if (!inFrame) {
        if (b!=0xFD) continue;
        inFrame=true; if (n<cap) out[n++]=b; decodedSeen=0; esc=false; continue;
      }
      if (n<cap) out[n++]=b;
      uint8_t d;
      if (esc) { d=b^0x80; esc=false; }
      else if (b==0xFC) { esc=true; continue; }
      else d=b;
      decodedSeen++;
      if (decodedSeen==1) lenHi=d;
      else if (decodedSeen==2) {
        packetLen=((uint16_t)lenHi<<8)|d;
        decodedNeed=2+packetLen+2;
        if (packetLen>500) return 0;
      }
      if (decodedNeed && decodedSeen>=decodedNeed) return n;
    }
    delay(1);
  }
  return n;
}

String extractAsciiTag(const uint8_t *enc, size_t n) {
  uint8_t dec[256]; size_t dn=0; bool esc=false;
  for (size_t i=0;i<n && dn<sizeof(dec);i++) {
    uint8_t b=enc[i];
    if (i==0 && b==0xFD) { dec[dn++]=b; continue; }
    if (esc) { dec[dn++]=b^0x80; esc=false; }
    else if (b==0xFC) esc=true;
    else dec[dn++]=b;
  }
  String s;
  for (size_t i=0;i<dn;i++) s += (dec[i]>=32 && dec[i]<=126) ? (char)dec[i] : '.';
  if (s.indexOf("Co_CPU_BL")>=0) return "Co_CPU_BL";
  if (s.indexOf("Co_CPU_App")>=0) return "Co_CPU_App";
  return "";
}

bool hmInitialise() {
  uint8_t buf[256]; while (HM.available()) HM.read();
  hmResetPulse();
  size_t n=hmReadEncodedFrame(buf,sizeof(buf),1200);
  String tag=extractAsciiTag(buf,n); if (tag.length()) moduleTag=tag;
  if (tag=="Co_CPU_BL") {
    const uint8_t changeApp[]={0x03};
    hmSendFrame(0x00,0x01,changeApp,sizeof(changeApp));
    hmReadEncodedFrame(buf,sizeof(buf),1000);
    n=hmReadEncodedFrame(buf,sizeof(buf),1500);
    tag=extractAsciiTag(buf,n); if (tag.length()) moduleTag=tag;
  }
  moduleReady=(moduleTag=="Co_CPU_App");
  return moduleReady;
}

bool sameEndpoint(const HbClient &c,const IPAddress &ip,uint16_t port){ return c.connected && c.ip==ip && c.port==port; }
HbClient* findClient(const IPAddress &ip,uint16_t port){ for(int i=0;i<MAX_HB_CLIENTS;i++) if(sameEndpoint(clients[i],ip,port)) return &clients[i]; return nullptr; }
void disconnectClient(HbClient &c){ if(c.connected){ statDisconnects++; c=HbClient(); } }
HbClient* findOrCreateClient(const IPAddress &ip,uint16_t port){
  HbClient *c=findClient(ip,port); if(c) return c;
  for(int i=0;i<MAX_HB_CLIENTS;i++) if(clients[i].connected && clients[i].ip==ip) disconnectClient(clients[i]);
  for(int i=0;i<MAX_HB_CLIENTS;i++) if(!clients[i].connected){ clients[i]=HbClient(); clients[i].ip=ip; clients[i].port=port; clients[i].endpointId=1; clients[i].lastRxMs=millis(); return &clients[i]; }
  return nullptr;
}

void hbSendTyped(HbClient &c,uint8_t type,const uint8_t *payload,size_t plen,int cntOverride=-1){
  uint8_t buf[1200]; if(plen+4>sizeof(buf)) return;
  uint8_t cnt=(cntOverride>=0)?(uint8_t)cntOverride:c.txCounter++;
  buf[0]=type; buf[1]=cnt; if(plen) memcpy(buf+2,payload,plen);
  uint16_t crc=crc16Bidcos(buf,2+plen); buf[2+plen]=(crc>>8)&0xFF; buf[3+plen]=crc&0xFF;
  hbUdp.beginPacket(c.ip,c.port); hbUdp.write(buf,plen+4); hbUdp.endPacket();
}

void handleHbPacket(const uint8_t *data,size_t len,const IPAddress &ip,uint16_t port){
  if(len<4) return;
  uint16_t calc=crc16Bidcos(data,len-2), got=((uint16_t)data[len-2]<<8)|data[len-1];
  if(calc!=got){ statBadCrc++; return; }
  uint8_t type=data[0], cnt=data[1]; const uint8_t *payload=data+2; size_t plen=len-4;
  HbClient *c=(type==T_CONNECT)?findOrCreateClient(ip,port):findClient(ip,port); if(!c) return;
  c->lastRxMs=millis();
  switch(type){
    case T_CONNECT:
      if(!c->connected){ c->connected=true; statConnects++; }
      c->started=false;
      if(plen==1 && payload[0]==1){ c->endpointId+=2; uint8_t resp[2]={1,cnt}; hbSendTyped(*c,T_CONNECT,resp,2); }
      else if(plen==2 && payload[0]==2){ uint8_t clientEp=payload[1]; if(clientEp==0){ c->endpointId+=2; c->started=false; } else c->endpointId=clientEp; uint8_t resp[3]={2,cnt,c->endpointId}; hbSendTyped(*c,T_CONNECT,resp,3); }
      break;
    case T_DISCONNECT: disconnectClient(*c); break;
    case T_KEEPALIVE: break;
    case T_LED: break;
    case T_RESET: hmResetPulse(); moduleTag="Co_CPU_BL"; moduleReady=false; break;
    case T_STARTCONN: c->started=true; break;
    case T_STOPCONN: c->started=false; break;
    case T_FRAME: if(plen){ HM.write(payload,plen); HM.flush(); statRxFrames++; } break;
    default: statUnknown++; break;
  }
}

void pollHbUdp(){
  int n=hbUdp.parsePacket(); if(n<=0) return;
  uint8_t buf[1200]; if(n>(int)sizeof(buf)){ while(hbUdp.available()) hbUdp.read(); return; }
  int rd=hbUdp.read(buf,n); if(rd>0) handleHbPacket(buf,rd,hbUdp.remoteIP(),hbUdp.remotePort());
}

void hbKeepalive(){
  static uint32_t lastKa=0; uint32_t now=millis(); if(now-lastKa<1000) return; lastKa=now;
  for(int i=0;i<MAX_HB_CLIENTS;i++){
    HbClient &c=clients[i]; if(!c.connected) continue;
    if(now-c.lastRxMs>5000){ statKeepaliveTimeout++; disconnectClient(c); continue; }
    hbSendTyped(c,T_KEEPALIVE,nullptr,0);
  }
}

void pumpHmUart(){
  if(!HM.available()) return;
  uint8_t buf[UART_CHUNK]; size_t n=0; uint32_t t0=micros();
  while(n<sizeof(buf) && (micros()-t0)<2500){
    while(HM.available() && n<sizeof(buf)) buf[n++]=(uint8_t)HM.read();
    if(n && !HM.available()) delayMicroseconds(150);
  }
  if(!n) return;
  for(int i=0;i<MAX_HB_CLIENTS;i++){ HbClient &c=clients[i]; if(c.connected && c.started){ hbSendTyped(c,T_FRAME,buf,n); statTxFrames++; } }
  if(rawClient && rawClient.connected()) rawClient.write(buf,n);
}

void pollRawTcp(){
  if(!rawClient || !rawClient.connected()){
    WiFiClient c=rawServer.available(); if(c){ if(rawClient) rawClient.stop(); rawClient=c; rawClient.setNoDelay(true); }
  }
  if(rawClient && rawClient.connected()){
    uint8_t buf[256]; int n=0;
    while(rawClient.available() && n<(int)sizeof(buf)){ int v=rawClient.read(); if(v<0) break; buf[n++]=(uint8_t)v; }
    if(n){ HM.write(buf,n); HM.flush(); }
  }
}

String htmlPage(){
  int active=0, started=0; for(int i=0;i<MAX_HB_CLIENTS;i++){ if(clients[i].connected) active++; if(clients[i].connected&&clients[i].started) started++; }
  String h; h.reserve(4500);
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>HM-C3 Dongle</title><style>body{font-family:system-ui;margin:24px;max-width:850px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #bbb;padding:7px;text-align:left}input{padding:8px;margin:4px;width:260px}button{padding:9px 14px;margin:4px}</style></head><body><h1>HM-MOD ESP32-C3 Dongle</h1><table>");
  h += "<tr><th>IP</th><td>"+WiFi.localIP().toString()+"</td></tr>";
  h += "<tr><th>Hostname</th><td>"+hostName+".local</td></tr>";
  h += "<tr><th>HB-RF-ETH</th><td>UDP "+String(HB_PORT)+"</td></tr>";
  h += "<tr><th>Raw UART</th><td>TCP "+String(RAW_TCP_PORT)+"</td></tr>";
  h += "<tr><th>HM Modul</th><td>"+(moduleTag.length()?moduleTag:String("unbekannt"))+"</td></tr>";
  h += "<tr><th>HB Clients</th><td>"+String(active)+" verbunden / "+String(started)+" gestartet</td></tr>";
  h += "<tr><th>Host->HM Frames</th><td>"+String(statRxFrames)+"</td></tr>";
  h += "<tr><th>HM->Host Pakete</th><td>"+String(statTxFrames)+"</td></tr>";
  h += "<tr><th>CRC Fehler</th><td>"+String(statBadCrc)+"</td></tr>";
  h += "<tr><th>Reset</th><td>"+String(statReset)+"</td></tr></table>";
  h += F("<h2>Aktionen</h2><form method='POST' action='/reset'><button>HM-Modul Reset</button></form><form method='POST' action='/reboot'><button>ESP neu starten</button></form><h2>WLAN</h2><form method='POST' action='/wifi'><input name='ssid' placeholder='SSID'><br><input name='pass' type='password' placeholder='Passwort'><br><button>Speichern & Neustart</button></form><p>RaspberryMatic/OpenCCU: IP dieses Dongles bei <b>IP-Adresse (HB-RF-ETH)</b> eintragen.</p></body></html>");
  return h;
}

void setupWeb(){
  web.on("/",HTTP_GET,[](){ web.send(200,"text/html; charset=utf-8",htmlPage()); });
  web.on("/api/status",HTTP_GET,[](){
    int active=0,started=0; for(int i=0;i<MAX_HB_CLIENTS;i++){ if(clients[i].connected)active++; if(clients[i].connected&&clients[i].started)started++; }
    String j="{\"ip\":\""+WiFi.localIP().toString()+"\",\"hostname\":\""+hostName+"\",\"module\":\""+moduleTag+"\",\"clients\":"+String(active)+",\"started\":"+String(started)+",\"rx\":"+String(statRxFrames)+",\"tx\":"+String(statTxFrames)+",\"crc_errors\":"+String(statBadCrc)+"}";
    web.send(200,"application/json",j);
  });
  web.on("/reset",HTTP_POST,[](){ hmResetPulse(); moduleTag="Co_CPU_BL"; moduleReady=false; web.sendHeader("Location","/"); web.send(303); });
  web.on("/reboot",HTTP_POST,[](){ web.send(200,"text/plain","Restart..."); delay(300); ESP.restart(); });
  web.on("/wifi",HTTP_POST,[](){
    if(!web.hasArg("ssid")){ web.send(400,"text/plain","SSID fehlt"); return; }
    prefs.begin("hmc3",false); prefs.putString("ssid",web.arg("ssid")); prefs.putString("pass",web.arg("pass")); prefs.end();
    web.send(200,"text/plain","Gespeichert. Neustart..."); delay(500); ESP.restart();
  });
  web.begin();
}

void startConfigAp(){
  uint64_t mac=ESP.getEfuseMac(); char suffix[5]; snprintf(suffix,sizeof(suffix),"%04X",(uint16_t)(mac&0xFFFF));
  String ap="HM-C3-"+String(suffix); WiFi.mode(WIFI_AP_STA); WiFi.softAP(ap.c_str());
  Serial.println("Setup-AP: "+ap); Serial.println("Setup-IP: "+WiFi.softAPIP().toString());
}

void connectWifi(){
  prefs.begin("hmc3",true); wifiSsid=prefs.getString("ssid",""); wifiPass=prefs.getString("pass",""); prefs.end();
  uint64_t mac=ESP.getEfuseMac(); char suffix[5]; snprintf(suffix,sizeof(suffix),"%04x",(uint16_t)(mac&0xFFFF)); hostName="hm-c3-"+String(suffix);
  WiFi.setHostname(hostName.c_str());
  if(wifiSsid.length()){
    WiFi.mode(WIFI_STA); WiFi.begin(wifiSsid.c_str(),wifiPass.c_str());
    uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000){ Serial.print('.'); delay(250); } Serial.println();
  }
  if(WiFi.status()!=WL_CONNECTED) startConfigAp(); else Serial.println("WLAN OK: "+WiFi.localIP().toString());
}

void setupMdns(){
  if(WiFi.status()!=WL_CONNECTED || !MDNS.begin(hostName.c_str())) return;
  MDNS.addService("hbrfeth","udp",HB_PORT); MDNS.addServiceTxt("hbrfeth","udp","wire","hb-rf-eth"); MDNS.addServiceTxt("hbrfeth","udp","model","HM-C3"); MDNS.addServiceTxt("hbrfeth","udp","radio","HM-MOD-RPI-PCB");
  MDNS.addService("rawuart","tcp",RAW_TCP_PORT); MDNS.addService("http","tcp",HTTP_PORT);
}

void setup(){
  Serial.begin(115200); delay(1200);
  Serial.println("\n====================================\n HM-MOD-RPI-PCB ESP32-C3 DONGLE\n HB-RF-ETH / UDP 3008\n====================================");
  HM.begin(HM_BAUD,SERIAL_8N1,HM_RX_PIN,HM_TX_PIN); pinMode(HM_RESET_PIN,INPUT_PULLUP);
  Serial.println("HM Selbsttest...");
  if(hmInitialise()) Serial.println("HM OK: "+moduleTag); else Serial.println("HM Selbsttest nicht eindeutig; Bridge startet trotzdem.");
  connectWifi(); hbUdp.begin(HB_PORT); rawServer.begin(); rawServer.setNoDelay(true); setupWeb(); setupMdns();
  Serial.println("HB-RF-ETH UDP: 3008\nRaw UART TCP : 2329\nWebUI        : http://"+hostName+".local/");
}

void loop(){ pollHbUdp(); hbKeepalive(); pumpHmUart(); pollRawTcp(); web.handleClient(); delay(1); }
