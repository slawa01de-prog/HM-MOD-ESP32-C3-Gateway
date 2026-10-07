
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Update.h>
#include <esp_system.h>

// ============================================================
// HM-MOD-RPI-PCB + ESP32-C3
// HB-RF-ETH network dongle for OpenCCU / RaspberryMatic
// Version 3.1 Pro: DualCoPro-aware + diagnostics + ESP OTA + HM updater
//
// Wiring:
// HM pin 10 TX  -> ESP GPIO6 RX
// HM pin 8  RX  <- ESP GPIO5 TX
// HM pin 12 RST <- ESP GPIO7
// HM pin 1 3V3  -> ESP 3V3
// HM GND        -> ESP GND
// ============================================================

#define HM_RX_PIN       6
#define HM_TX_PIN       5
#define HM_RESET_PIN    7
#define HM_BAUD         115200

#define HB_PORT         3008
#define HTTP_PORT       80
#define MAX_HB_CLIENTS  4

#define T_CONNECT       0
#define T_DISCONNECT    1
#define T_KEEPALIVE     2
#define T_LED           3
#define T_RESET         4
#define T_STARTCONN     5
#define T_STOPCONN      6
#define T_FRAME         7

#define HM_DST_HMSYSTEM 0x00
#define HM_DST_TRX      0x01
#define HM_DST_HMIP     0x02
#define HM_DST_LLMAC    0x03
#define HM_DST_COMMON   0xFE
#define HM_DST_OS       HM_DST_HMSYSTEM

#define HM_CMD_HMSYSTEM_IDENTIFY     0x00
#define HM_CMD_HMSYSTEM_GET_VERSION  0x02
#define HM_CMD_HMSYSTEM_CHANGE_APP   0x03
#define HM_CMD_HMSYSTEM_ACK          0x04
#define HM_CMD_HMSYSTEM_GET_SERIAL   0x0B
#define HM_CMD_UPDATE_FW             0x05

#define HM_CMD_TRX_GET_VERSION       0x02
#define HM_CMD_TRX_ACK               0x04
#define HM_CMD_TRX_GET_MCU_TYPE      0x09
#define HM_CMD_TRX_GET_DEFAULT_RF_ADDR 0x10

#define HM_CMD_HMIP_GET_DEFAULT_RF_ADDR 0x01
#define HM_CMD_HMIP_ACK              0x06

#define HM_CMD_LLMAC_ACK             0x01
#define HM_CMD_LLMAC_GET_SERIAL      0x07
#define HM_CMD_LLMAC_GET_DEFAULT_RF_ADDR 0x08

#define HM_CMD_COMMON_IDENTIFY       0x01
#define HM_CMD_COMMON_START_BL       0x02
#define HM_CMD_COMMON_START_APP      0x03
#define HM_CMD_COMMON_GET_SGTIN      0x04
#define HM_CMD_COMMON_ACK            0x05

#define FW_PATH         "/hmfw.eq3"
#define MAX_FW_BLOCK    2048

HardwareSerial HM(1);
WiFiUDP hbUdp;
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

String wifiSsid;
String wifiPass;
String hostName;

String moduleTag = "unknown";
String moduleFirmware = "unknown";
String moduleSerial = "unknown";
String moduleSGTIN = "unknown";
String moduleTypeName = "HM-MOD-RPI-PCB";
uint8_t moduleMcuType = 0;
uint32_t moduleHmIpAddress = 0;
uint32_t moduleBidCosAddress = 0;
String lastHostFrame = "";
String lastHmFrame = "";
uint32_t lastHmFrameMs = 0;
uint32_t lastHostFrameMs = 0;

uint32_t statConnects = 0;
uint32_t statDisconnects = 0;
uint32_t statBadCrc = 0;
uint32_t statUnknown = 0;
uint32_t statHostToHm = 0;
uint32_t statHmToHost = 0;
uint32_t statResets = 0;
uint32_t statKeepaliveTimeout = 0;
uint32_t statUartFrames = 0;
uint32_t statUartFrameDrops = 0;
uint32_t statResetOpenCCU = 0;
uint32_t statResetWeb = 0;
uint32_t statResetStartup = 0;
uint32_t statResetUpdater = 0;
uint32_t statWifiReconnects = 0;
uint32_t lastWifiOkMs = 0;

#define SYSLOG_LINES 48
#define SYSLOG_WIDTH 112
char sysLog[SYSLOG_LINES][SYSLOG_WIDTH];
uint8_t sysLogHead = 0;
uint8_t sysLogCount = 0;

volatile bool maintenanceMode = false;
volatile bool flashBusy = false;
bool fwUploaded = false;
size_t fwUploadedBytes = 0;
int fwBlockCount = 0;
int flashPercent = 0;
String flashStatus = "Bereit";
String flashError = "";
bool rebootPending = false;
uint32_t rebootAt = 0;

// ============================================================
// CRC16 - poly 0x8005, init 0xD77F
// ============================================================
uint16_t crc16Bidcos(const uint8_t *data, size_t len) {
  uint16_t crc = 0xD77F;
  while (len--) {
    crc ^= ((uint16_t)(*data++)) << 8;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x8005);
      else crc = (uint16_t)(crc << 1);
    }
  }
  return crc;
}

String hexShort(const uint8_t *data, size_t len, size_t maxBytes = 64) {
  String s;
  size_t n = min(len, maxBytes);
  s.reserve(n * 3 + 8);
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02X", data[i]);
    if (i) s += ' ';
    s += b;
  }
  if (len > n) s += " ...";
  return s;
}


String formatUptime() {
  uint32_t sec = millis() / 1000;
  uint32_t days = sec / 86400; sec %= 86400;
  uint32_t hrs = sec / 3600; sec %= 3600;
  uint32_t min = sec / 60; sec %= 60;
  char b[48];
  snprintf(b, sizeof(b), "%ud %02u:%02u:%02u", days, hrs, min, sec);
  return String(b);
}

String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_SW: return "Software";
    case ESP_RST_PANIC: return "Panic";
    case ESP_RST_INT_WDT: return "Interrupt-WDT";
    case ESP_RST_TASK_WDT: return "Task-WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_DEEPSLEEP: return "DeepSleep";
    default: return "Andere";
  }
}

void addLog(const String &msg) {
  char line[SYSLOG_WIDTH];
  snprintf(line, sizeof(line), "[%10lu ms] %s", (unsigned long)millis(), msg.c_str());
  strncpy(sysLog[sysLogHead], line, SYSLOG_WIDTH - 1);
  sysLog[sysLogHead][SYSLOG_WIDTH - 1] = 0;
  sysLogHead = (sysLogHead + 1) % SYSLOG_LINES;
  if (sysLogCount < SYSLOG_LINES) sysLogCount++;
  Serial.println(line);
}

String htmlEscape(String v) {
  v.replace("&", "&amp;");
  v.replace("<", "&lt;");
  v.replace(">", "&gt;");
  v.replace("\"", "&quot;");
  return v;
}

String hexAddress24(uint32_t a) {
  if (!a) return "unknown";
  char b[16];
  snprintf(b, sizeof(b), "0x%06lX", (unsigned long)(a & 0xFFFFFF));
  return String(b);
}

// ============================================================
// HM reset
// active LOW
// ============================================================
void hmResetPulse(const char *source = "Local") {
  pinMode(HM_RESET_PIN, OUTPUT);
  digitalWrite(HM_RESET_PIN, LOW);
  delay(50);
  digitalWrite(HM_RESET_PIN, HIGH);
  delay(50);
  statResets++;
  if (!strcmp(source, "OpenCCU")) statResetOpenCCU++;
  else if (!strcmp(source, "Web")) statResetWeb++;
  else if (!strcmp(source, "Startup")) statResetStartup++;
  else if (!strcmp(source, "Updater")) statResetUpdater++;
  moduleTag = "Co_CPU_BL";
  addLog(String("HM reset: ") + source);
}

// ============================================================
// HM wire parser for normal HB-RF-ETH operation.
// Keeps encoded frame bytes exactly as sent by module.
// ============================================================
class HmWireParser {
public:
  static const size_t CAP = 2048;

  enum State {
    NO_DATA,
    LEN_HI,
    LEN_LO,
    FRAME_DATA
  };

  uint8_t buf[CAP];
  size_t pos = 0;
  State state = NO_DATA;
  bool escaped = false;
  uint16_t frameLen = 0;
  uint16_t framePos = 0;

  void reset() {
    pos = 0;
    state = NO_DATA;
    escaped = false;
    frameLen = 0;
    framePos = 0;
  }

  bool push(uint8_t chr) {
    if (chr == 0xFD) {
      pos = 0;
      escaped = false;
      state = LEN_HI;
      frameLen = 0;
      framePos = 0;
      buf[pos++] = chr;
      return false;
    }

    if (state == NO_DATA) return false;

    if (pos >= CAP) {
      reset();
      statUartFrameDrops++;
      return false;
    }

    buf[pos++] = chr;

    if (chr == 0xFC && !escaped) {
      escaped = true;
      return false;
    }

    uint8_t decoded = chr;
    if (escaped) {
      decoded = chr | 0x80;
      escaped = false;
    }

    switch (state) {
      case LEN_HI:
        frameLen = ((uint16_t)decoded) << 8;
        state = LEN_LO;
        break;

      case LEN_LO:
        frameLen |= decoded;
        frameLen += 2; // CRC
        framePos = 0;
        if (frameLen > 1900) {
          reset();
          statUartFrameDrops++;
        } else {
          state = FRAME_DATA;
        }
        break;

      case FRAME_DATA:
        framePos++;
        if (framePos >= frameLen) {
          state = NO_DATA;
          return true;
        }
        break;

      default:
        break;
    }

    return false;
  }
};

HmWireParser hmParser;

// ============================================================
// Low-level decoded frame reader for updater.
// Output buffer contains unescaped bytes including:
// FD lenHi lenLo dst cnt payload... crcHi crcLo
// ============================================================
bool readHmDecodedFrame(uint8_t *out, size_t cap, size_t &outLen, uint32_t timeoutMs) {
  outLen = 0;
  bool started = false;
  bool escaped = false;
  size_t totalNeed = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (HM.available()) {
      uint8_t b = (uint8_t)HM.read();

      if (!started) {
        if (b != 0xFD) continue;
        started = true;
        outLen = 0;
        if (cap < 1) return false;
        out[outLen++] = 0xFD;
        escaped = false;
        continue;
      }

      if (b == 0xFD && !escaped) {
        // New frame start; resync.
        outLen = 0;
        out[outLen++] = 0xFD;
        totalNeed = 0;
        escaped = false;
        continue;
      }

      if (b == 0xFC && !escaped) {
        escaped = true;
        continue;
      }

      if (escaped) {
        b |= 0x80;
        escaped = false;
      }

      if (outLen >= cap) return false;
      out[outLen++] = b;

      if (outLen == 3) {
        uint16_t packetLen = ((uint16_t)out[1] << 8) | out[2];
        totalNeed = (size_t)packetLen + 5;
        if (totalNeed > cap || packetLen > 3000) return false;
      }

      if (totalNeed && outLen >= totalNeed) {
        uint16_t calc = crc16Bidcos(out, outLen - 2);
        uint16_t got = ((uint16_t)out[outLen - 2] << 8) | out[outLen - 1];
        return calc == got;
      }
    }

    delay(1);
    yield();
  }

  return false;
}

void hmSendEscaped(uint8_t b) {
  if (b == 0xFC || b == 0xFD) {
    HM.write(0xFC);
    HM.write(b & 0x7F);
  } else {
    HM.write(b);
  }
}

void hmSendCommand(uint8_t dst, uint8_t cnt, const uint8_t *payload, size_t plen) {
  uint16_t packetLen = (uint16_t)(plen + 2);

  uint8_t head[5];
  head[0] = 0xFD;
  head[1] = (uint8_t)(packetLen >> 8);
  head[2] = (uint8_t)(packetLen & 0xFF);
  head[3] = dst;
  head[4] = cnt;

  // CRC over unescaped frame without CRC bytes.
  uint8_t raw[2100];
  if (plen + 5 > sizeof(raw)) return;

  memcpy(raw, head, 5);
  if (plen) memcpy(raw + 5, payload, plen);
  uint16_t crc = crc16Bidcos(raw, plen + 5);

  HM.write(0xFD);
  for (int i = 1; i < 5; i++) hmSendEscaped(head[i]);
  for (size_t i = 0; i < plen; i++) hmSendEscaped(payload[i]);
  hmSendEscaped((uint8_t)(crc >> 8));
  hmSendEscaped((uint8_t)(crc & 0xFF));
  HM.flush();
}

bool frameHasAscii(const uint8_t *frame, size_t len, const char *needle) {
  size_t nl = strlen(needle);
  if (nl == 0 || len < nl) return false;

  for (size_t i = 0; i + nl <= len; i++) {
    bool ok = true;
    for (size_t j = 0; j < nl; j++) {
      if (frame[i + j] != (uint8_t)needle[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

bool detectModeTagInFrame(const uint8_t *frame, size_t len, String &tag) {
  const char *tags[] = {"DualCoPro_App", "HMIP_TRX_App", "HMIP_TRX_Bl", "Co_CPU_App", "Co_CPU_BL"};
  for (const char *t : tags) {
    if (frameHasAscii(frame, len, t)) {
      tag = t;
      return true;
    }
  }
  return false;
}

bool waitForBootTag(String &tag, uint32_t timeoutMs) {
  uint8_t frame[2300];
  size_t len = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    uint32_t remain = timeoutMs - (millis() - start);
    if (!readHmDecodedFrame(frame, sizeof(frame), len, min((uint32_t)500, remain))) continue;
    if (detectModeTagInFrame(frame, len, tag)) return true;
  }
  return false;
}

bool isBootMode(const String &tag) {
  return tag == "Co_CPU_BL" || tag == "HMIP_TRX_Bl";
}

bool isAppMode(const String &tag) {
  return tag == "Co_CPU_App" || tag == "DualCoPro_App" || tag == "HMIP_TRX_App";
}

bool identifyMode(String &tag, uint32_t timeoutMs = 1800) {
  uint8_t frame[2300];
  size_t len = 0;
  uint8_t cnt = 0x60;

  while (HM.available()) HM.read();

  uint8_t cmd = HM_CMD_COMMON_IDENTIFY;
  hmSendCommand(HM_DST_COMMON, cnt++, &cmd, 1);
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, sizeof(frame), len, 500)) continue;
    if (detectModeTagInFrame(frame, len, tag)) return true;
  }

  cmd = HM_CMD_HMSYSTEM_IDENTIFY;
  hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  start = millis();
  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, sizeof(frame), len, 500)) continue;
    if (detectModeTagInFrame(frame, len, tag)) return true;
  }

  return false;
}

bool enterBootloader() {
  while (HM.available()) HM.read();
  hmResetPulse("Updater");

  String tag;
  if (waitForBootTag(tag, 2200)) {
    moduleTag = tag;
    if (isBootMode(tag)) return true;
  }

  if (!identifyMode(tag, 1500)) return false;
  moduleTag = tag;
  if (isBootMode(tag)) return true;

  uint8_t frame[2300];
  size_t len = 0;
  uint8_t cnt = 0x70;
  uint8_t cmd;

  if (tag == "DualCoPro_App" || tag == "HMIP_TRX_App") {
    cmd = HM_CMD_COMMON_START_BL;
    hmSendCommand(HM_DST_COMMON, cnt++, &cmd, 1);
  } else if (tag == "Co_CPU_App") {
    cmd = HM_CMD_HMSYSTEM_CHANGE_APP;
    hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  } else {
    return false;
  }

  readHmDecodedFrame(frame, sizeof(frame), len, 1200);
  if (waitForBootTag(tag, 3000) && isBootMode(tag)) {
    moduleTag = tag;
    delay(700);
    return true;
  }
  return false;
}

bool enterApplication() {
  String tag;
  if (identifyMode(tag, 1500)) {
    moduleTag = tag;
    if (isAppMode(tag)) return true;
  } else {
    // On a freshly reset module the unsolicited boot banner may be the only clue.
    if (waitForBootTag(tag, 1000)) {
      moduleTag = tag;
      if (isAppMode(tag)) return true;
    }
  }

  uint8_t frame[2300];
  size_t len = 0;
  uint8_t cnt = 0x80;
  uint8_t cmd;

  // HM-MOD-RPI-PCB bootloader identifies as Co_CPU_BL even when the
  // installed application is the 2.8.6 DualCoPro firmware.
  if (moduleTag == "Co_CPU_BL") {
    cmd = HM_CMD_HMSYSTEM_CHANGE_APP;
    hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  } else if (moduleTag == "HMIP_TRX_Bl") {
    cmd = HM_CMD_COMMON_START_APP;
    hmSendCommand(HM_DST_COMMON, cnt++, &cmd, 1);
  } else {
    // Try legacy start command as last resort for HM-MOD-RPI-PCB.
    cmd = HM_CMD_HMSYSTEM_CHANGE_APP;
    hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  }

  readHmDecodedFrame(frame, sizeof(frame), len, 1200); // usually ACK

  if (waitForBootTag(tag, 3500) && isAppMode(tag)) {
    moduleTag = tag;
    addLog("HM application: " + tag);
    delay(400);
    return true;
  }

  // Some firmwares answer IDENTIFY instead of pushing a banner.
  if (identifyMode(tag, 1800) && isAppMode(tag)) {
    moduleTag = tag;
    addLog("HM application identified: " + tag);
    return true;
  }

  return false;
}

bool requestHmFrame(uint8_t dst, uint8_t cmd, uint8_t expectedDst, uint8_t expectedCmd,
                    uint8_t *frame, size_t cap, size_t &len, uint32_t timeoutMs = 1800) {
  static uint8_t counter = 0xA0;
  while (HM.available()) HM.read();
  hmSendCommand(dst, counter++, &cmd, 1);
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, cap, len, 500)) continue;
    if (len >= 8 && frame[3] == expectedDst && frame[5] == expectedCmd) return true;
  }
  return false;
}

bool queryFirmwareVersion(String &versionOut) {
  uint8_t frame[2300];
  size_t len = 0;

  if (moduleTag == "DualCoPro_App" || moduleTag == "HMIP_TRX_App") {
    if (requestHmFrame(HM_DST_TRX, HM_CMD_TRX_GET_VERSION,
                       HM_DST_TRX, HM_CMD_TRX_ACK, frame, sizeof(frame), len)) {
      if (len >= 12 && frame[6] == 0x01) {
        char v[20];
        snprintf(v, sizeof(v), "%u.%u.%u", frame[7], frame[8], frame[9]);
        versionOut = v;
        moduleFirmware = versionOut;
        return true;
      }
    }
  }

  // Legacy Co_CPU_App fallback.
  if (requestHmFrame(HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_GET_VERSION,
                     HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_ACK, frame, sizeof(frame), len)) {
    if (len >= 15 && frame[6] == 0x02) {
      char v[20];
      snprintf(v, sizeof(v), "%u.%u.%u", frame[10], frame[11], frame[12]);
      versionOut = v;
      moduleFirmware = versionOut;
      return true;
    }
  }

  return false;
}

bool queryModuleInfo() {
  if (!isAppMode(moduleTag) && !enterApplication()) return false;

  uint8_t frame[2300];
  size_t len = 0;
  bool any = false;

  String version;
  if (queryFirmwareVersion(version)) any = true;

  if (moduleTag == "DualCoPro_App" || moduleTag == "HMIP_TRX_App") {
    if (requestHmFrame(HM_DST_TRX, HM_CMD_TRX_GET_MCU_TYPE,
                       HM_DST_TRX, HM_CMD_TRX_ACK, frame, sizeof(frame), len) &&
        len >= 10 && frame[6] == 0x01) {
      moduleMcuType = frame[7];
      if (moduleMcuType == 3) moduleTypeName = "HM-MOD-RPI-PCB";
      else if (moduleMcuType == 4) moduleTypeName = "RPI-RF-MOD";
      else if (moduleMcuType == 1) moduleTypeName = "HmIP-RFUSB";
      any = true;
    }

    if (requestHmFrame(HM_DST_HMIP, HM_CMD_HMIP_GET_DEFAULT_RF_ADDR,
                       HM_DST_HMIP, HM_CMD_HMIP_ACK, frame, sizeof(frame), len) &&
        len >= 11 && frame[6] == 0x01) {
      moduleHmIpAddress = ((uint32_t)frame[7] << 16) | ((uint32_t)frame[8] << 8) | frame[9];
      any = true;
    }

    if (requestHmFrame(HM_DST_COMMON, HM_CMD_COMMON_GET_SGTIN,
                       HM_DST_COMMON, HM_CMD_COMMON_ACK, frame, sizeof(frame), len) &&
        len >= 20 && frame[6] == 0x01) {
      char b[32];
      snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
               frame[7], frame[8], frame[9], frame[10], frame[11], frame[12],
               frame[13], frame[14], frame[15], frame[16], frame[17], frame[18]);
      moduleSGTIN = b;
      any = true;
    }

    if (requestHmFrame(HM_DST_LLMAC, HM_CMD_LLMAC_GET_DEFAULT_RF_ADDR,
                       HM_DST_LLMAC, HM_CMD_LLMAC_ACK, frame, sizeof(frame), len) &&
        len >= 11 && frame[6] == 0x01) {
      moduleBidCosAddress = ((uint32_t)frame[7] << 16) | ((uint32_t)frame[8] << 8) | frame[9];
      any = true;
    }

    if (requestHmFrame(HM_DST_LLMAC, HM_CMD_LLMAC_GET_SERIAL,
                       HM_DST_LLMAC, HM_CMD_LLMAC_ACK, frame, sizeof(frame), len) &&
        len >= 18 && frame[6] == 0x01) {
      char b[11];
      memcpy(b, frame + 7, 10);
      b[10] = 0;
      moduleSerial = b;
      any = true;
    }
  } else {
    if (requestHmFrame(HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_GET_SERIAL,
                       HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_ACK, frame, sizeof(frame), len) &&
        len >= 18 && frame[6] == 0x02) {
      char b[11];
      memcpy(b, frame + 7, 10);
      b[10] = 0;
      moduleSerial = b;
      any = true;
    }
  }

  if (any) {
    addLog("HM info: " + moduleTypeName + " FW " + moduleFirmware + " SN " + moduleSerial);
  }
  return any;
}

// ============================================================
// .eq3 parser / validator
//
// Format used by HM-MOD-UART firmware:
// 4 ASCII hex chars = block length
// followed by blockLength*2 ASCII hex chars.
// ============================================================
int hexNibble(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool readHexByte(File &f, uint8_t &out) {
  int a = f.read();
  int b = f.read();
  if (a < 0 || b < 0) return false;

  int hi = hexNibble(a);
  int lo = hexNibble(b);
  if (hi < 0 || lo < 0) return false;

  out = (uint8_t)((hi << 4) | lo);
  return true;
}

bool readBlockLength(File &f, uint16_t &blockLen) {
  int c[4];
  for (int i = 0; i < 4; i++) {
    c[i] = f.read();
    if (c[i] < 0) return false;
    if (hexNibble(c[i]) < 0) return false;
  }

  blockLen =
      ((uint16_t)hexNibble(c[0]) << 12) |
      ((uint16_t)hexNibble(c[1]) << 8)  |
      ((uint16_t)hexNibble(c[2]) << 4)  |
      ((uint16_t)hexNibble(c[3]));

  return true;
}

bool validateFirmwareFile(const char *path, int &blocks, size_t &decodedBytes, String &error) {
  blocks = 0;
  decodedBytes = 0;
  error = "";

  File f = SPIFFS.open(path, FILE_READ);
  if (!f) {
    error = "Firmware-Datei kann nicht geöffnet werden.";
    return false;
  }

  while (f.available()) {
    uint16_t blockLen = 0;
    if (!readBlockLength(f, blockLen)) {
      error = "Ungültige Blocklänge/Hex-Daten.";
      f.close();
      return false;
    }

    if (blockLen < 2 || blockLen > MAX_FW_BLOCK) {
      error = "Ungültige Blockgröße: " + String(blockLen);
      f.close();
      return false;
    }

    for (uint16_t i = 0; i < blockLen; i++) {
      uint8_t dummy;
      if (!readHexByte(f, dummy)) {
        error = "Firmware-Datei endet mitten in Block " + String(blocks + 1);
        f.close();
        return false;
      }
    }

    blocks++;
    decodedBytes += blockLen;

    if (blocks > 4096) {
      error = "Zu viele Firmware-Blöcke.";
      f.close();
      return false;
    }
  }

  f.close();

  if (blocks == 0) {
    error = "Keine Firmware-Blöcke gefunden.";
    return false;
  }

  return true;
}

bool waitUpdateAck(uint32_t timeoutMs) {
  uint8_t frame[2300];
  size_t len = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, sizeof(frame), len, 600)) continue;

    // flash-hmmoduart expects payload 04 01 on OS destination.
    if (len >= 9 &&
        frame[3] == HM_DST_OS &&
        frame[5] == HM_CMD_HMSYSTEM_ACK &&
        frame[6] == 0x01) {
      return true;
    }
  }

  return false;
}

void disconnectAllHbClients() {
  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    clients[i] = HbClient();
  }
}

bool flashFirmwareNow() {
  addLog("HM firmware flash requested");
  flashBusy = true;
  maintenanceMode = true;
  flashError = "";
  flashPercent = 0;
  flashStatus = "OpenCCU-Verbindung gesperrt";
  disconnectAllHbClients();

  while (hbUdp.parsePacket() > 0) {
    while (hbUdp.available()) hbUdp.read();
  }

  int blocks = 0;
  size_t decodedBytes = 0;
  String err;

  if (!validateFirmwareFile(FW_PATH, blocks, decodedBytes, err)) {
    flashError = err;
    flashStatus = "Abbruch";
    flashBusy = false;
    maintenanceMode = false;
    return false;
  }

  fwBlockCount = blocks;
  flashStatus = "HM-MOD Bootloader wird gestartet";

  if (!enterBootloader()) {
    flashError = "Co_CPU_BL konnte nicht erreicht werden.";
    flashStatus = "Abbruch";
    flashBusy = false;
    maintenanceMode = false;
    return false;
  }

  File f = SPIFFS.open(FW_PATH, FILE_READ);
  if (!f) {
    flashError = "Firmware-Datei kann nicht geöffnet werden.";
    flashStatus = "Abbruch";
    flashBusy = false;
    maintenanceMode = false;
    return false;
  }

  static uint8_t blockData[MAX_FW_BLOCK];
  static uint8_t payload[MAX_FW_BLOCK + 1];
  uint8_t counter = 0;

  for (int block = 0; block < blocks; block++) {
    uint16_t blockLen = 0;

    if (!readBlockLength(f, blockLen) ||
        blockLen < 2 ||
        blockLen > MAX_FW_BLOCK) {
      flashError = "Lesefehler bei Block " + String(block + 1);
      f.close();
      flashStatus = "Abbruch";
      flashBusy = false;
      maintenanceMode = false;
      return false;
    }

    for (uint16_t i = 0; i < blockLen; i++) {
      if (!readHexByte(f, blockData[i])) {
        flashError = "Hex-Lesefehler bei Block " + String(block + 1);
        f.close();
        flashStatus = "Abbruch";
        flashBusy = false;
        maintenanceMode = false;
        return false;
      }
    }

    // Mirror flash-hmmoduart:
    // original block length -> cmd length = blockLen - 1
    // payload[0] = 0x05 (OS_UPDATE_FIRMWARE)
    // then first blockLen-2 bytes from firmware block;
    // final two firmware bytes are not included.
    size_t payloadLen = blockLen - 1;
    payload[0] = HM_CMD_UPDATE_FW;
    if (blockLen > 2) memcpy(payload + 1, blockData, blockLen - 2);

    while (HM.available()) HM.read();

    hmSendCommand(HM_DST_OS, counter++, payload, payloadLen);

    flashStatus = "Block " + String(block + 1) + " / " + String(blocks);
    flashPercent = (int)(((uint32_t)(block + 1) * 100U) / (uint32_t)blocks);

    if (!waitUpdateAck(2500)) {
      flashError = "Kein ACK 04 01 bei Block " + String(block + 1);
      f.close();
      flashStatus = "Abbruch";
      flashBusy = false;
      maintenanceMode = false;
      return false;
    }

    yield();
  }

  f.close();

  flashStatus = "Firmware übertragen - starte Application";
  delay(1000);

  if (!enterApplication()) {
    flashError = "Firmware wurde übertragen, Application (Co_CPU_App/DualCoPro_App) startet aber nicht.";
    flashStatus = "Prüfung fehlgeschlagen";
    flashBusy = false;
    maintenanceMode = true;
    return false;
  }

  if (!queryModuleInfo()) {
    flashError = "Application läuft, Modulinfo konnte aber nicht vollständig gelesen werden.";
    flashStatus = "Prüfung unvollständig";
    flashBusy = false;
    maintenanceMode = true;
    return false;
  }

  flashPercent = 100;
  flashStatus = "Erfolgreich - Firmware " + moduleFirmware + " / " + moduleTag;
  addLog("HM firmware flash OK: " + moduleFirmware + " / " + moduleTag);
  flashBusy = false;

  // Keep maintenance active until ESP reboots so OpenCCU cannot interrupt
  // the verified post-flash state.
  maintenanceMode = true;
  rebootPending = true;
  rebootAt = millis() + 7000;

  return true;
}

// ============================================================
// HB-RF-ETH
// ============================================================
bool sameEndpoint(const HbClient &c, const IPAddress &ip, uint16_t port) {
  return c.connected && c.ip == ip && c.port == port;
}

HbClient* findClient(const IPAddress &ip, uint16_t port) {
  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    if (sameEndpoint(clients[i], ip, port)) return &clients[i];
  }
  return nullptr;
}